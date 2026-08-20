 ;/**
 * @file HAS1_tagmachine_main.ino
 * @author 김병준 (you@domain.com)
 * @brief
 * @version 1.0
 * @date 2022-11-29
 *
 * @copyright Copyright (c) 2022
 *
 */

#define FIRMWARE_VER 7
#include "HAS1_tagmachine_main.h"
// #include <esp_task_wdt.h>  // [WDT 비활성화]

void setup() {
    Serial.begin(115200);
    // OTA 프레임(최대 512바이트 payload+7바이트 오버헤드)이 몰려도 유실 없이 받도록 여유 있게 확보.
    toSubSerial.setRxBufferSize(1024);
    toMainSerial.setRxBufferSize(1024);
    // 9600은 태그 문자열 정도는 충분하지만 OTA로 수백 KB 펌웨어를 릴레이하기엔 너무 느려(수십 분)
    // Beetle(HAS1_tagmachine_sub)과 함께 상향했다 — 양쪽 보레이트는 반드시 같아야 한다.
    toSubSerial.begin(115200, SERIAL_8N1, SUB_BEETLE_RX_PIN, SUB_BEETLE_TX_PIN);
    toMainSerial.begin(115200, SERIAL_8N1, MAIN_BEETLE_RX_PIN, MAIN_BEETLE_TX_PIN);
    NeopixelInit();
    TimerInit();
    // Mp3_Setup();  // [DFPlayer 비활성화]
    pinMode(RELAY_PIN, OUTPUT);
//  has2wifi.Setup("city");
    // badland 테마: 라이브러리가 주변 badland_* 후보(badland_ruins/auto/shoot) 중
    // RSSI가 가장 센 AP로 자동 연결하고, 서버 호스트도 badland용(172.30.1.43)으로 맞춘다.
    has2wifi.Setup("badland");
    // 현재 펌웨어 버전을 서버 device.esp_version 컬럼에 보고 (부팅 시 1회)
    has2wifi.Send((String)(const char*)my["device_name"], "esp_version", String(FIRMWARE_VER));
    TelnetInit();
    ota.setLogStream(Serial);
    ota.setOnSuccess([]() {
        has2wifi.Send((String)(const char*)my["device_name"], "device_state", "setting");
    });
    ota.setOnSkip([]() {
        has2wifi.Send((String)(const char*)my["device_name"], "device_state", "setting");
    });
    DataChanged();
    GameSetting();

    // Main Beetle 핸드셰이크 테스트 (최대 10초)
    Serial.println("=== Main Beetle Handshake Start ===");
    unsigned long hsStart = millis();
    bool hsSuccess = false;
    while(millis() - hsStart < 10000) {
        // Main Beetle에서 들어오는 'W' 확인 후 응답
        if(toMainSerial.available() > 0) {
            String cmd = toMainSerial.readStringUntil('\n');
            Serial.println("Main Beetle RX: " + cmd);
            if(cmd[0] == 'W') {
                toMainSerial.println("W");
                Serial.println("Main Beetle TX: W sent");
            }
            while(toMainSerial.available()) toMainSerial.read();
        }
        delay(100);
    }
    Serial.println("=== Main Beetle Handshake End ===");

    // setup() 완료 시점에도 ptrCurrentMode가 설정되지 않았으면 WaitFunc로 안전하게 초기화
    if (ptrCurrentMode == nullptr) {
        ptrCurrentMode = WaitFunc;
        Serial.println("[WARN] ptrCurrentMode was nullptr → set to WaitFunc");
    }
    // [WDT 비활성화] DFPlayer 원인 분리 테스트용 주석처리
    // esp_task_wdt_deinit();
    // {
    //     esp_task_wdt_config_t wdt_cfg = { .timeout_ms = 12000, .idle_core_mask = 0, .trigger_panic = true };
    //     esp_task_wdt_init(&wdt_cfg);
    // }
    // esp_task_wdt_add(NULL);
    // Serial.println("[WDT] 12s watchdog started");
}
void loop() {
    // esp_task_wdt_reset();  // [WDT 비활성화]
    if (ptrCurrentMode != nullptr) ptrCurrentMode();
    TimerRun();
    TelnetRun();
}
