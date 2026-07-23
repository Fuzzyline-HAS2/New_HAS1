#include "library_and_pin.h"

// ── Queue 실체 정의 ──────────────────────────────────────────────────────────
// 선언은 HAS1_itembox.h(extern) — 정의는 여기서만
QueueHandle_t sendQueue;     // Core1 → Core0  (GameEventSend → HTTP POST)
QueueHandle_t receiveQueue;  // Core0 → Core1  (ReceiveMine 결과 → DataChanged)
StaticJsonDocument<1000> myDoc;  // Core1 소유 JSON — xQueueReceive 후 deserialize

// has2wifi 객체 (Core0 태스크 전용)
HAS2_Wifi has2wifi("http://172.30.1.43");

// ── Core 0 태스크 ─────────────────────────────────────────────────────────────
// Core 1의 loop()와 물리적으로 분리 — MaintainWifi 블로킹이 게임 루프에 무영향
void WifiTaskFunc(void*) {
    for (;;) {
        // MaintainWifi + shift_machine HTTP GET (재연결 시 수 초 블로킹 가능 — Core0이므로 OK)
        has2wifi.Loop();

        if ((int)shift_machine["shift_machine"] >= 1) {
            has2wifi.ReceiveMine();   // my JSON 갱신 (Core0 임시 소유)
            char buf[512];
            serializeJson(my, buf, sizeof(buf));          // 문자열화
            xQueueSend(receiveQueue, buf, 0);             // Core1으로 전달 (논블로킹)
        }

        // watchdog 플래그: 서버가 재시작을 요청하면 ACK 후 ESP.restart()
        if ((int)shift_machine["watchdog"] >= 1) {
            has2wifi.Send((const char*)my["device_name"], "watchdog", "0");
            ESP.restart();
        }

        // Core1의 송신 요청 소비 → HTTP POST (블로킹 OK, Core0이므로)
        SendMsg msg;
        while (xQueueReceive(sendQueue, &msg, 0))
            has2wifi.Send(msg.deviceName, msg.key, msg.value);
    }
}

// ── Core1에서 setup()에서만 호출 ──────────────────────────────────────────────
void WifiInit() {
    sendQueue    = xQueueCreate(8, sizeof(SendMsg));
    receiveQueue = xQueueCreate(4, 512);

    has2wifi.Setup("badland");   // 스캔·연결 (블로킹 — setup() 안이므로 OK)
    has2wifi.Send((const char*)my["device_name"], "esp_version", "30");

    xTaskCreatePinnedToCore(WifiTaskFunc, "wifi", 8192, NULL, 1, NULL, 0);
    Log("NET", "wifi task pinned to Core 0");
}
