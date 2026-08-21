#define FIRMWARE_VER 3
#define PARTITION_VER 2
#include "HAS1_itembox.h"

// Runnable 플래그 갱신 — loop() 가장 먼저 호출. 이 순간의 millis()를 기준으로 통일.
void TickRunnables() {
    uint32_t now = millis();
    auto tick = [&](Runnable& r) {
        r.due = (now - r.lastRun >= r.periodMs);
        if (r.due) r.lastRun = now;
    };
    tick(motorR); tick(encoderR); tick(blinkR); tick(rfidR);
}

void setup() {
    Serial.begin(115200);
    Log("MAIN", "boot");
    vibration_motor_Init();  // GPIO14 부팅 HIGH 방지 — 반드시 가장 먼저
    MotorInit();
    EncoderInit();
    NeopixelInit();
    delay(1000);
    RfidInit();
    delay(1000);
    WifiInit();              // has2wifi.Setup() + Queue 생성 + Core0 태스크 시작
    TelnetInit();            // Telnet 서버 시작 (WiFi는 WifiInit이 완료한 뒤)
    Mp3_Init();              // 내부 2초 delay — 맨 마지막
    ChangeGameState(GAME_SETTING);
    Log("MAIN", "setup done");
}

void loop() {
    // [1] 타이밍 판단 — 이 순간의 millis()로 모든 due 플래그 통일
    TickRunnables();

    // [2] HAL 실행 — 시간 트리거, 최우선
    if (motorR.due)   MotorHalUpdate();
    if (encoderR.due) EncoderHalUpdate();
    if (blinkR.due)   BlinkHalUpdate();
    if (rfidR.due)    RfidHalUpdate();

    // [3] 서버 수신 소비 — Core0이 직렬화해 넣은 JSON 수신, Core1에서만 my 역직렬화
    char jsonBuf[512];
    if (xQueueReceive(receiveQueue, jsonBuf, 0)) {
        deserializeJson(myDoc, jsonBuf);
        DataChanged();
    }

    // [4] 게임 상태 머신
    GameUpdate();

    // [5] 진단 로그 미러링
    TelnetRun();
}
