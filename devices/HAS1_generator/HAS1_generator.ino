#include <HAS2_Wifi.h>
#include <SecureOTA.h>
#include <nvs_flash.h>
#include <WiFi.h>

/**
 * @file Done_Generator_code.ino
 * @author 김병준 (you@domain.com)
 * @brief
 * @version 1.0
 * @date 2022-11-29
 *
 * @copyright Copyright (c) 2022
 *
 */

// FIRMWARE_VER / PARTITION_VER: OTA가 "새 버전이 있는지" 판단하는 기준값.
// GitHub Releases에 올라간 version.txt / partition_version.txt와 비교되며,
// 배포할 때마다 이 숫자를 올려야 기기가 업데이트를 인식한다.
#define FIRMWARE_VER 11
#define PARTITION_VER 1
#include "HAS1_generator.h"

// ---------------------------------------------------------------------------------
// setup(): 전원이 켜지거나 리셋될 때 1회만 실행되는 초기화 루틴.
// 각 모듈(네오픽셀/RFID/엔코더/배선/DFPlayer/타이머)을 순서대로 초기화한 뒤,
// WiFi 연결과 OTA 콜백을 등록하고, 마지막으로 게임 상태 머신을 대기 상태로 세팅한다.
// ---------------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    NeopixelInit();  // 네오픽셀 4개 스트립 초기화 (전체 흰색 점등)
    RfidInit();      // PN532 RFID 리더 초기화
    EncoderInit();   // 엔코더용 하드웨어 펄스 카운터(PCNT) 초기화
    WireInit();      // 배선 감지 핀 4개 INPUT_PULLUP 설정
    Mp3_Init();      // DFPlayer(MP3) 모듈 초기화
    TimerInit();     // SimpleTimer 3종 등록(및 정지)

    // NVS(비휘발성 저장소) 파티션을 지우고 재초기화 — WiFi 등 라이브러리가 저장한
    // 이전 상태(예: 예전 접속 정보)를 남기지 않고 항상 깨끗하게 시작하기 위함.
    nvs_flash_erase();
    nvs_flash_init();

    // "badland" 테마로 연결 — HAS2_Wifi 라이브러리가 badland_ruins/badland_auto/badland_shoot
    // 후보 AP 중 신호가 가장 좋은 곳으로 자동 접속하고, 서버 호스트도 badland용(172.30.1.43)으로 설정된다.
    // 연결 직후 현재 펌웨어 버전을 서버에 보고한다.
    has2wifi.Setup("badland");

    // ---- [실험용 디버깅] 어느 AP/서브넷에 붙었는지 확인용 (taewoo.kim 요청) ----
    // GW가 172.30.1.1이 아니면 다른 망, GW는 맞는데 BSSID가 3C:78:95:E9:A0:0F 계열이 아니면
    // 같은 대역을 쓰는 다른 매장/테마 AP에 붙은 것으로 의심. ESP IP가 172.30.1.130/.131
    // 근처면 VLAN 분리 여부도 확인 필요.
    Serial.printf("IP=%s GW=%s MASK=%s\n",
        WiFi.localIP().toString().c_str(),
        WiFi.gatewayIP().toString().c_str(),
        WiFi.subnetMask().toString().c_str());
    Serial.printf("SSID=%s BSSID=%s RSSI=%d\n",
        WiFi.SSID().c_str(), WiFi.BSSIDstr().c_str(), WiFi.RSSI());
    // ---------------------------------------------------------------------

    has2wifi.Send((String)(const char*)my["device_name"], "esp_version", String(FIRMWARE_VER));

    TelnetInit(); // Telnet 서버 시작 (WiFi 연결 완료 후) — 이후 Serial.* 출력은 telnet.ino로 미러링됨

    // OTA 진행 로그를 시리얼로 출력하도록 설정
    ota.setLogStream(Serial);

    // OTA 성공 시: 서버에 "setting" 상태를 알리고 재부팅 시작을 로그로 남김
    ota.setOnSuccess([]() {
        has2wifi.Send((String)(const char*)my["device_name"], "device_state", "setting");
        Serial.println("[OTA] update succees! Start rebooting...");
    });
    // OTA 스킵(이미 최신 버전) 시: 마찬가지로 "setting" 상태를 알림
    ota.setOnSkip([]() {
        has2wifi.Send((String)(const char*)my["device_name"], "device_state", "setting");
        Serial.println("[OTA] Already up-to-date version.");
    });

    // 파티션 테이블(partitions.bin) 자체를 갱신해야 하는 경우를 위한 별도 OTA 채널.
    // PARTITION_VER이 서버의 partition_version.txt보다 낮을 때만 적용된다.
    ota.setPartitionUpdate(
        "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_generator/partitions.bin",
        "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_generator/partitions.sig",
        "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_generator/partition_version.txt",
        PARTITION_VER
    );

    // 게임 상태 머신을 "아무 것도 안 함" 상태로 초기화
    ptrCurrentMode = WaitFunc;
    ptrRfidMode = WaitFunc;

    // 서버로부터 받아온 초기 데이터(my)를 한 번 처리해서 현재 game_state/device_state에 맞는
    // 모드로 즉시 전환되도록 함 (예: 이미 game_state가 "activate"라면 ActivateFunc가 바로 호출됨)
    DataChanged();
}

// ---------------------------------------------------------------------------------
// loop(): 전원이 켜져 있는 동안 반복 실행되는 메인 루프.
// 매 프레임마다 (1) 엔코더 하드웨어 카운터를 읽어 반영하고,
// (2) 현재 게임 모드에 해당하는 함수를 실행하고,
// (3) 등록된 SimpleTimer들을 갱신한다.
// 블로킹(delay 등)을 최소화해 WiFi 통신·네오픽셀 갱신·엔코더 반영이 최대한 끊기지 않게 한다.
// ---------------------------------------------------------------------------------
void loop() {
    TelnetRun();        // Telnet 클라이언트 접속/데이터 처리
    EncoderLoop();     // PCNT 하드웨어 카운터 → encoderValue 반영
    ptrCurrentMode();  // 현재 모드 함수 실행 (예: WaitFunc / RfidLoopMain / WirePollMain / StarterActivate)
    // battery_max/starter_finish/repaired 단계는 ptrCurrentMode가 WirePollMain이 아니라서(BatteryFinish/
    // StarterActivate/WaitFunc 등) 배선 폴링이 끊긴다 - 그 사이 배선을 뽑아 배터리팩을 재사용하는 부정행위를
    // 잡기 위해 ptrCurrentMode와 무관하게 매 프레임 별도로 감시한다.
    WireTheftMonitorLoop();
    TimerRun();         // WifiTimer / GameTimer / BlinkTimer 갱신 — 인터벌이 도래하면 콜백 실행
}
