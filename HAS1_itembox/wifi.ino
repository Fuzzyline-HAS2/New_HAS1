#include "library_and_pin.h"

// ── Queue 실체 정의 ──────────────────────────────────────────────────────────
// 선언은 HAS1_itembox.h(extern) — 정의는 여기서만
QueueHandle_t sendQueue;          // Core1 → Core0  (GameEventSend → HTTP POST)
QueueHandle_t receiveQueue;       // Core0 → Core1  (ReceiveMine 결과 → DataChanged)
QueueHandle_t roleRequestQueue;   // Core1 → Core0  (tagUser → Receive 요청)
QueueHandle_t roleResponseQueue;  // Core0 → Core1  (role 결과 반환)
StaticJsonDocument<1000> myDoc;   // Core1 소유 JSON — xQueueReceive 후 deserialize

// has2wifi·http·tag 객체 — Core0 WifiTaskFunc 전용, Core1 직접 접근 금지
// HTTPClient http 는 HAS2_Wifi.cpp 전역 싱글턴(비스레드세이프)
HAS2_Wifi has2wifi("http://172.30.1.43");

// ── Core 0 태스크 — 모든 has2wifi.* 호출은 여기서만 ──────────────────────────
void WifiTaskFunc(void*) {
    for (;;) {
        // MaintainWifi + HTTP GET shift_machine + (shift>=1이면 ReceiveMine 포함)
        has2wifi.Loop();

        // shift_machine >= 1 → Loop()가 ReceiveMine() 호출 완료, my 갱신됨
        // → 직렬화 후 Core1으로 전달
        if ((int)shift_machine["shift_machine"] >= 1) {
            char buf[512];
            serializeJson(my, buf, sizeof(buf));
            xQueueSend(receiveQueue, buf, 0);
        }

        // role 조회 요청 처리 (Core1 ActivateState에서 투입)
        char tagUserBuf[32];
        if (xQueueReceive(roleRequestQueue, tagUserBuf, 0)) {
            has2wifi.Receive(tagUserBuf);  // HTTP GET — Core0 전용, 안전
            char roleBuf[32];
            strlcpy(roleBuf, (const char*)tag["role"], 32);
            xQueueSend(roleResponseQueue, roleBuf, 0);
        }

        // Core1의 송신 요청 소비 → HTTP POST
        SendMsg msg;
        while (xQueueReceive(sendQueue, &msg, 0))
            has2wifi.Send(msg.deviceName, msg.key, msg.value);
    }
}

// ── Core1에서 호출 — 논블로킹 게임 이벤트 송신 ──────────────────────────────
void GameEventSend(const char* key, const char* value) {
    SendMsg msg;
    strlcpy(msg.deviceName, myDoc["device_name"] | "", 32);
    strlcpy(msg.key,   key,   32);
    strlcpy(msg.value, value, 64);
    if (xQueueSend(sendQueue, &msg, 0) != pdTRUE)
        Log("GAME", String("event DROPPED (queue full): ") + key + "=" + value);
    else
        Log("GAME", String("event queued: ") + key + "=" + value);
}

// ── Core1에서 호출 — role 조회 요청 (논블로킹, 즉시 리턴) ───────────────────
void WifiRequestPlayer(const char* tagUser) {
    char buf[32];
    strlcpy(buf, tagUser, 32);
    xQueueOverwrite(roleRequestQueue, buf);  // 이전 미처리 요청 덮어씀
}

// ── Core1에서 호출 — role 조회 결과 폴링 (논블로킹) ─────────────────────────
// Core0이 Receive() 완료 전이면 "" 반환 → 다음 루프에서 재시도
String WifiPollPlayerRole() {
    char roleBuf[32];
    if (xQueueReceive(roleResponseQueue, roleBuf, 0)) return String(roleBuf);
    return "";
}

// ── Core1에서 setup()에서만 호출 ──────────────────────────────────────────────
void WifiInit() {
    sendQueue         = xQueueCreate(8, sizeof(SendMsg));
    receiveQueue      = xQueueCreate(4, 512);
    roleRequestQueue  = xQueueCreate(1, 32);
    roleResponseQueue = xQueueCreate(1, 32);

    // badland 외 AP로 연결했던 이력 제거 — 저장된 SK_DA20_2.4G 시도로 스캔 실패 방지
    Preferences prefs;
    prefs.begin("has2wifi", false);
    prefs.remove("last_ssid");
    prefs.remove("last_pw");
    prefs.end();

    has2wifi.Setup("badland");
    has2wifi.Send((const char*)my["device_name"], "esp_version", "30");

    xTaskCreatePinnedToCore(WifiTaskFunc, "wifi", 8192, NULL, 1, NULL, 0);
    Log("NET", "wifi task pinned to Core 0");
}
