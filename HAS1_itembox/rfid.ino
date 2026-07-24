#include "rfid_hal.h"
#include "library_and_pin.h"

Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS1);

// HAL 내부 캐시 — RfidHalUpdate()만 쓰고, RfidTagPresent/ReadTag는 읽기만 한다
static bool    rfid_tagPresent = false;
static bool    rfid_dataReady  = false;
static uint8_t rfid_tagData[32];

void RfidInit() {
    nfc.begin();
    if (!nfc.getFirmwareVersion())
        Log("RFID", "PN532 connect FAIL");
    else {
        nfc.SAMConfig();
        Log("RFID", "PN532 online");
        NeoSetAll(YELLOW);
    }
}

// 200ms Runnable 호출 전용. 태그 감지 + 데이터 읽기를 캐시에 저장.
// 호출 빈도 제어는 rfidR.due가 담당 — 함수 내부에 millis() 없음.
void RfidHalUpdate() {
    byte buf[64] = {0};
    if (!nfc.sendCommandCheckAck(buf, 1)) {
        rfid_tagPresent = rfid_dataReady = false;
        return;
    }
    rfid_tagPresent = nfc.startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A);

    if (rfid_tagPresent && !rfid_dataReady) {
        // 태그 있고 아직 미읽음 → 데이터 읽기
        rfid_dataReady = nfc.ntag2xx_ReadPage(7, rfid_tagData);
    }
    if (!rfid_tagPresent) {
        rfid_dataReady = false;  // 태그 빠지면 캐시 초기화
    }
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

