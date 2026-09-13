void TimerInit(){
    wifiTimerId = WifiTimer.setInterval(2000,WifiIntervalFunc);
    gameTimerId = GameTimer.setInterval(500,GameTimerFunc);
    // Beetle 읽기는 게임 상태와 무관하게 항상 빠르게 돈다.
    beetleTimerId = BeetleTimer.setInterval(100,CommnunicationBeetle);
    GameTimer.disable(gameTimerId);
}

void WifiIntervalFunc(){
    // WiFi 재연결 감지: 끊겼다가 다시 연결되면 stable state 재적용
    static bool lastWifiConnected = false;
    bool nowConnected = (WiFi.status() == WL_CONNECTED);
    if (!lastWifiConnected && nowConnected) {
        Serial.println("[WIFI] Reconnected.");
    }
    lastWifiConnected = nowConnected;

    has2wifi.Loop(DataChanged);
    // CommnunicationBeetle()은 BeetleTimer(100ms)가 전담한다. 여기서 또 부르면
    // 2초마다 HTTP 왕복 직후에 한 번 더 읽는 것뿐이라 의미가 없다.
    FlushPendingTagSend(); // GameTimer 비활성 구간(태그 전) 등 유실 방지용 안전망
    HandleRuntimeRecovery(); // bad event 누적 + 모터 timeout 감시 (silence 제외)
}

void GameTimerFunc(){
    CommnunicationBeetle();
    Serial.print("Tag Count:");
    Serial.println(tagCnt);
    HandleRuntimeRecovery(); // bad event 누적 + 모터 timeout 감시 (silence 제외)
    ptrCurrentMode(); // TagCount() 오디오 재생 — tagged_players 전송(HTTP)보다 먼저 실행
    FlushPendingTagSend(); // 오디오 재생 이후에 서버로 상태 전송
    // 여기 있던 버퍼 드레인을 제거했다. CommnunicationBeetle()이 이미 버퍼의 모든 줄을
    // 처리하므로 남은 것은 그 이후 도착한 유효한 줄이고, 버리면 다음 주기에 놓친다.
}
