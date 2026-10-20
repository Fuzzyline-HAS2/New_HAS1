#include "library_and_pin.h"

// ── Queue / 전역 실체 정의 ───────────────────────────────────────────────────
// 선언은 HAS1_itembox.h(extern) — 정의는 여기서만
QueueHandle_t sendQueue;          // Core1 → Core0  (GameEventSend → HTTP POST)
QueueHandle_t receiveQueue;       // Core0 → Core1  (ReceiveMine 결과 → DataChanged)
QueueHandle_t roleRequestQueue;   // Core1 → Core0  (tagUser → Receive 요청)
QueueHandle_t roleResponseQueue;  // Core0 → Core1  (role 결과 반환)
StaticJsonDocument<1000> myDoc;   // Core1 소유 JSON — xQueueReceive 후 deserialize

volatile bool otaRequested = false;  // Core1이 세팅, Core0이 소비

// has2wifi·ota·tag 객체 — Core0 WifiTaskFunc 전용, Core1 직접 접근 금지
HAS2_Wifi has2wifi("http://172.30.1.43");

SecureOTA ota(
    "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_itembox/update.bin",
    "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_itembox/version.txt",
    "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_itembox/update.sig",
    HMAC_SECRET,
    FIRMWARE_VER
);

// ── Core 0 태스크 — 모든 has2wifi.* / ota.* 호출은 여기서만 ─────────────────
void WifiTaskFunc(void*) {
    for (;;) {
        // OTA 트리거 확인 — ota.check()는 블로킹, 성공 시 ESP.restart() 호출
        if (otaRequested) {
            otaRequested = false;
            ota.check();
        }

        // MaintainWifi + HTTP GET shift_machine + (shift>=1이면 ReceiveMine 포함)
        has2wifi.Loop();

        // shift_machine >= 1 → Loop()가 ReceiveMine() 호출 완료, my 갱신됨
        // → 직렬화 후 Core1으로 전달
        if ((int)shift_machine["shift_machine"] >= 1) {
            char buf[512];
            serializeJson(my, buf, sizeof(buf));
            xQueueSend(receiveQueue, buf, 0);
        }

        // role 조회 요청 처리 (Core1 ActivateState/PausedState/TaggerState에서 투입)
        char tagUserBuf[32];
        if (xQueueReceive(roleRequestQueue, tagUserBuf, 0)) {
            tag.clear();                   // HTTP 실패 시 직전 글러브의 role이 남아 있지 않게
            has2wifi.Receive(tagUserBuf);  // HTTP GET — Core0 전용, 안전
            const char* role = tag["role"] | "";
            // 미등록 태그·HTTP 실패로 role이 비면 "unknown"으로 돌려준다 — ""는 Core1이
            // "아직 응답 없음"으로 읽어 pendingTagUser가 영원히 안 풀린다.
            if (role[0] == '\0') role = "unknown";
            // 어느 태그에 대한 답인지 "<tagUser>:<role>"로 함께 돌려줘, 상태가 바뀐 뒤 늦게
            // 도착한 이전 요청의 응답을 Core1이 새 태그의 결과로 오인하지 않게 한다.
            char roleBuf[32];
            snprintf(roleBuf, sizeof(roleBuf), "%s:%s", tagUserBuf, role);
            xQueueOverwrite(roleResponseQueue, roleBuf);  // depth=1 — 최신 응답만 남긴다
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
// Core0이 Receive() 완료 전이면 "" 반환 → 다음 루프에서 재시도.
// 응답은 "<tagUser>:<role>" — 묻고 있는 태그(tagUser)의 답만 돌려주고, 다른 태그의
// 늦은 응답은 버린다.
String WifiPollPlayerRole(const String& tagUser) {
    char roleBuf[32];
    if (!xQueueReceive(roleResponseQueue, roleBuf, 0)) return "";
    String resp(roleBuf);
    int sep = resp.indexOf(':');
    if (sep < 0 || resp.substring(0, sep) != tagUser) {
        Log("WIFI", "stale role response dropped: " + resp + " (waiting " + tagUser + ")");
        return "";
    }
    return resp.substring(sep + 1);
}

// ── Core1에서 setup()에서만 호출 ──────────────────────────────────────────────
void WifiInit() {
    sendQueue         = xQueueCreate(8, sizeof(SendMsg));
    receiveQueue      = xQueueCreate(4, 512);
    roleRequestQueue  = xQueueCreate(1, 32);
    roleResponseQueue = xQueueCreate(1, 32);

    // 콜드부트 시 WiFi 라디오 웜업 — 라이브러리 TryConnect 3초 타임아웃 우회
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    for (unsigned long t = millis(); WiFi.status() != WL_CONNECTED && millis() - t < 15000;)
        delay(100);
    // 이미 AP 인증 완료 상태이므로 Setup 내부 disconnect→reconnect가 즉시 성공.
    // badland 테마: 주변 badland_* 후보(badland_ruins/auto/shoot) 중 RSSI가 가장 센 AP로
    // 자동 연결하고, 서버 호스트도 badland용(172.30.1.43)으로 맞춘다 (HAS1_generator와 동일).
    // 웜업은 WIFI_SSID(badland_ruins, badland 후보 중 하나)로 미리 붙여 Setup 내부
    // disconnect→reconnect가 즉시 끝나게 해주므로, 테마로 바꿔도 그대로 유효하다.
    has2wifi.Setup("badland");
    has2wifi.Send((const char*)my["device_name"], "esp_version", String(FIRMWARE_VER).c_str());

    // SecureOTA 콜백 — Core0 컨텍스트에서 실행되므로 has2wifi.Send() 직접 호출 가능
    ota.setOnSuccess([]() {
        has2wifi.Send((const char*)my["device_name"], "device_state", "setting");
    });
    ota.setOnSkip([]() {
        has2wifi.Send((const char*)my["device_name"], "device_state", "setting");
    });
    ota.setPartitionUpdate(
        "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_itembox/partitions.bin",
        "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_itembox/partitions.sig",
        "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_itembox/partition_version.txt",
        PARTITION_VER
    );

    xTaskCreatePinnedToCore(WifiTaskFunc, "wifi", 8192, NULL, 1, NULL, 0);
    Log("NET", "wifi task pinned to Core 0");
}
