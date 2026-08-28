void TimerInit(){
    wifiTimerId = WifiTimer.setInterval(2000,WifiIntervalFunc);
    gameTimerId = GameTimer.setInterval(500,GameTimerFunc);
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
    CommnunicationBeetle();
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
    while(toSubSerial.available()){
        toSubSerial.read();
    }
}
