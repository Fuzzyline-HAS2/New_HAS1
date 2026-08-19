#include "hal.h"
#include "library_and_pin.h"

Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS1);

// HAL 내부 캐시 — RfidHalUpdate()만 쓰고, RfidTagPresent/ReadTag는 읽기만 한다
static bool    rfid_tagPresent = false;
static bool    rfid_dataReady  = false;
static uint8_t rfid_tagData[32];

// ── PN532 근접 인식 Dead Zone 대응 — RxGain 동적 전환 ───────────────────────
// 일부 생산 로트의 PN532는 기본 RxGain(38dB)에서 태그를 안테나 중심에 맞춰 대면
// 약 2cm 이하 근거리에서 인식이 안 되는 특성이 있음이 실측으로 확인됨
// (동일 TTGO/배선/태그/펌웨어에서 PN532만 교체해 재현 — 로트별 RF 편차, MCU/통신 문제 아님).
// RxGain을 낮추면(23dB) 근거리(~2cm)가, 기본보다 높이면(33dB) 중거리(2~4cm)가 각각
// 커버되므로, 두 세팅을 상황에 따라 전환해 근접~4cm 전체 구간을 잇는다.
// TX 출력(GsNOn/CWGsP)은 실측상 근거리 개선 기여가 낮아 PN532 기본값을 그대로 둔다.
// (GainMode enum 정의는 hal.h — Arduino 자동 프로토타입 생성 때문에 여기 두면 안 됨)

static GainMode      currentGain     = GAIN_NEAR;
static bool          rfid_tagLocked  = false;   // 태그를 찾아 유지 중인지 (탐색 모드 vs 유지 모드)
static uint8_t       rfid_lockedData[32];        // 유지 중인 태그의 page7 데이터 — 동일 태그 판별 기준
static unsigned long rfid_lastSeenMs = 0;        // 유지 중 태그를 마지막으로 확인한 시각

// 유지 중이던 태그가 두 Gain 모두에서 이 시간 이상 연속으로 안 잡히면 그제서야 제거로 판정.
// (단 한 번의 Read 실패로 바로 태그 제거 처리하지 않기 위한 디바운스 — 500~1000ms 범위에서 조정 가능)
#define TAG_REMOVE_TIME_MS 500

// RFConfiguration(0x32) CfgItem 0x0A(Type A 106kbps Analog Setting)로 RxGain을 전환한다.
// PN532는 이 설정을 내부에 영구 저장하지 않으므로, 초기화/재초기화 때마다 다시 적용해야 한다.
// 매개변수를 GainMode가 아닌 int로 받는다 — Arduino가 .ino 탭들을 병합할 때 자동 생성하는
// 함수 프로토타입은 실제 코드보다도 앞에 삽입돼서, 커스텀 enum을 매개변수로 쓰면 "타입을
// 아직 모른다"는 컴파일 에러가 난다(enum은 int로 암묵 변환되므로 호출부는 그대로 둬도 됨).
static bool ApplyGain(int mode) {
    uint8_t rfCfg = (mode == GAIN_NEAR) ? 0x19 : 0x49;  // 23dB(근거리) / 33dB(중거리)
    uint8_t cmd[] = {
        0x32,       // RFConfiguration
        0x0A,       // Type A 106kbps Analog Setting
        rfCfg,      // RFCfg — RxGain (아래 TX 관련 값들은 실측상 기본값 유지가 최선이었음)
        0xF4,       // GsNOn
        0x3F,       // CWGsP
        0x11,       // ModGsP
        0x4D,       // Demod RF ON
        0x85,       // RxThreshold
        0x61,       // Demod RF OFF
        0x6F,       // GsNOff
        0x26,       // ModWidth
        0x62,       // MifNFC
        0x87        // TxBitPhase
    };
    return nfc.sendCommandCheckAck(cmd, sizeof(cmd), 1000);
}

// 현재 칩에 적용된 Gain으로 태그 감지 + page7 읽기를 1회 시도한다.
// 성공 시 outData에 태그 데이터(4바이트 "GxPx" 등)를 채우고 true를 반환한다.
static bool DetectAndRead(uint8_t outData[32]) {
    byte buf[64] = {0};
    if (!nfc.sendCommandCheckAck(buf, 1)) return false;
    if (!nfc.startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A)) return false;
    return nfc.ntag2xx_ReadPage(7, outData);
}

void RfidInit() {
    nfc.begin();
    if (!nfc.getFirmwareVersion())
        Log("RFID", "PN532 connect FAIL");
    else {
        nfc.SAMConfig();
        Log("RFID", "PN532 online");
    }
    // PN532는 RF 설정을 저장하지 않으므로 초기화(전원 재투입/모듈 교체 포함)마다 재적용한다.
    currentGain = GAIN_NEAR;
    ApplyGain(currentGain);
    rfid_tagLocked = false;
}

// 200ms Runnable 호출 전용. 태그 감지 + 데이터 읽기를 캐시에 저장.
// 호출 빈도 제어는 rfidR.due가 담당 — "얼마나 자주 부를지"는 함수 내부에서 재지 않는다.
// (TAG_REMOVE_TIME_MS 판정에 쓰는 millis()는 "얼마나 오래 유지했는지" 계산용으로 별개)
//
// 탐색 모드(태그 미보유): 현재 Gain으로 1회 시도 → 실패하면 반대 Gain으로 즉시 재시도
//   (23dB↔33dB를 오가며 근접~4cm 전 구간을 커버, 둘 다 실패하면 이번 tick은 미검출)
// 유지 모드(태그 보유): 현재 Gain으로 먼저 확인 → 실패하면 반대 Gain으로 즉시 재확인
//   → 그래도 둘 다 실패하면 TAG_REMOVE_TIME_MS 동안은 유지로 간주(단발성 미스 무시)
//   → 유예시간 초과 시에만 최종적으로 태그 제거 판정, 이후 탐색 모드로 복귀
void RfidHalUpdate() {
    uint8_t data[32];

    if (!rfid_tagLocked) {
        if (!DetectAndRead(data)) {
            currentGain = (currentGain == GAIN_NEAR) ? GAIN_FAR : GAIN_NEAR;
            ApplyGain(currentGain);
            if (!DetectAndRead(data)) {
                rfid_tagPresent = rfid_dataReady = false;
                return;
            }
        }
        // 태그 발견 — 지금 이 Gain을 유지하며 락온
        rfid_tagLocked = true;
        memcpy(rfid_lockedData, data, 32);
        memcpy(rfid_tagData, data, 32);
        rfid_lastSeenMs = millis();
        rfid_tagPresent = rfid_dataReady = true;
        return;
    }

    bool found = DetectAndRead(data) && memcmp(data, rfid_lockedData, 32) == 0;
    if (!found) {
        GainMode otherGain = (currentGain == GAIN_NEAR) ? GAIN_FAR : GAIN_NEAR;
        ApplyGain(otherGain);
        if (DetectAndRead(data) && memcmp(data, rfid_lockedData, 32) == 0) {
            currentGain = otherGain;  // 반대 Gain에서 같은 태그 재확인 → 그 Gain으로 전환해 유지
            found = true;
        } else {
            ApplyGain(currentGain);   // 재확인 실패 — 칩 설정을 원래 Gain으로 되돌려 상태 일치시킴
        }
    }

    if (found) {
        rfid_lastSeenMs = millis();
        rfid_tagPresent = true;
        if (!rfid_dataReady) {  // 이미 소비된 상태였다면 재갱신 (동일 락온 데이터라 재읽기 불필요)
            memcpy(rfid_tagData, rfid_lockedData, 32);
            rfid_dataReady = true;
        }
        return;
    }

    if (millis() - rfid_lastSeenMs < TAG_REMOVE_TIME_MS) {
        rfid_tagPresent = true;  // 유예시간 이내 — 단발성 미스로 보고 유지 상태 유지
        return;
    }

    // 유예시간 초과 — 태그 제거 확정, 탐색 모드로 복귀
    rfid_tagLocked = false;
    rfid_tagPresent = rfid_dataReady = false;
    currentGain = GAIN_NEAR;
    ApplyGain(currentGain);
}

// 캐시된 값 반환 — 하드웨어 접근 없음, loop()마다 안전하게 호출 가능
bool RfidTagPresent() {
    return rfid_tagPresent;
}

// 캐시된 데이터 반환. 소비 마킹(dataReady=false)으로 동일 태그 중복 소비 방지.
// 다음 RfidHalUpdate(200ms)에서 태그 유지 중이면 자동 재읽기.
bool RfidReadTag(uint8_t data[32]) {
    if (!rfid_dataReady) return false;
    memcpy(data, rfid_tagData, 32);
    rfid_dataReady = false;
    return true;
}
