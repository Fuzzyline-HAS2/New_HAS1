// =================================================================================
// timer.ino
// ---------------------------------------------------------------------------------
// SimpleTimer 기반 3개 인터벌(WifiTimer/GameTimer/BlinkTimer)의 초기화와 콜백을 모아놓은 파일.
// TimerRun()이 매 loop마다 세 타이머를 갱신(run)하고, 각 인터벌이 도래하면 아래 콜백들이 호출된다.
// GameTimer/BlinkTimer는 상시 동작하는 게 아니라 필요할 때만 deleteTimer/setInterval로 켜고 끈다.
// =================================================================================

// setup()에서 1회 호출: 세 인터벌을 등록한 뒤, GameTimer/BlinkTimer는 곧바로 정지시킨다
// (WifiTimer만 상시 폴링, 나머지 둘은 게임 진행 상황에 따라 필요할 때 다시 켜짐).
// 태스크 워치독도 여기서 함께 등록한다 — 배선을 다양한 device_state에서 빠르게 뺐다 꽂았다
// 반복하면 loop()가 멈춰버려 물리 리셋 버튼을 눌러야만 복구되던 문제 대응. 30초 안에
// TimerRun()의 esp_task_wdt_reset()이 안 불리면(=loop()가 멈춤) 자동으로 재부팅되고,
// CrashReportInit()이 그 직전 BREADCRUMB 위치를 다음 부팅 때 로그로 남긴다.
void TimerInit(){
    static const esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = 30000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&wdt_cfg);
    esp_task_wdt_add(NULL);  // 현재 태스크(loop) 등록

    wifiTimerId = WifiTimer.setInterval(wifiTime,WifiIntervalFunc);
    gameTimerId = GameTimer.setInterval(gameTime,GameTimerFunc);

    blinkTimerId = BlinkTimer.setInterval(blinkTime,BlinkTimerFunc);

    GameTimer.deleteTimer(gameTimerId);

    BlinkTimer.deleteTimer(blinkTimerId);
}

// WifiTimer 콜백 (wifiTime = 2000ms마다 실행): 서버와 통신해 my/tag 등의 JSON을 갱신하고,
// 변경분이 있으면 DataChanged() 콜백을 통해 게임 상태 전환을 처리한다.
void WifiIntervalFunc(){
    // 2초마다 찍히는 브레드크럼이라 다른 함수 브레드크럼이 안 남았어도 "직전에 어느
    // device_state였는지"는 최소 2초 해상도로 남는다 - "어느 상태에서 멈췄는지" 질문에
    // 직접 답하기 위함.
    char stateBuf[40];
    snprintf(stateBuf, sizeof(stateBuf), "Wifi:ds=%s", (const char *)my["device_state"]);
    BREADCRUMB(stateBuf);
    has2wifi.Loop(DataChanged);
    CrashReportSend((const char *)my["device_name"]);
    CrashNvsFlush();
}

// GameTimer 콜백 (gameTime = 400ms마다 실행): 스타터 진행 중 손잡이를 돌리지 않고 방치하면
// 게이지가 서서히 줄어들도록 encoderValue를 깎는다.
// gameTimerCnt가 5에 도달할 때(400ms*5=2초)마다 starterDecreaseAmount만큼 감소시키고,
// 감소를 유지한 채(gameTimerCnt=3) 다음 2초 뒤 다시 감소하도록 리셋값을 3으로 잡는다.
// encoderValue가 0 밑으로 내려가면 0으로 고정하고 카운터도 0으로 리셋한다.
void GameTimerFunc(){
    gameTimerCnt++;
    // Serial.println("gameTimerCnt:" + (String)gameTimerCnt);
    if(gameTimerCnt == 5){ // 0.5s x 6 =3sec
        encoderValue = encoderValue - starterDecreaseAmount;
        gameTimerCnt = 3;
        if(encoderValue < 0){
            encoderValue = 0;
            gameTimerCnt = 0;
        }
    }
}

// BlinkTimer 콜백 (blinkTime = 1800ms마다 실행): blinkNeo 스트립을 blinkColor 색과 꺼짐(BLACK) 사이로
// 토글한다. blinkOn 플래그로 현재 어느 상태인지 기억한다.
void BlinkTimerFunc(){
    // Serial.println("Blink!");
    if(blinkOn == true){
        NeoLightColor(blinkNeo, color[blinkColor]);
        blinkOn = false;
    }
    else{
        NeoLightColor(blinkNeo, color[BLACK]);
        blinkOn = true;
    }
}

// 지정한 네오픽셀(Neo)을 지정한 색(NeoColor)으로 깜빡이기 시작한다.
// blinkNeo/blinkColor 전역을 갱신하고 BlinkTimer 인터벌을 (재)등록한다.
void BlinkTimerStart(int Neo, int NeoColor){
    blinkNeo = Neo;
    blinkColor = NeoColor;
    blinkTimerId = BlinkTimer.setInterval(blinkTime, BlinkTimerFunc);
}

// loop()에서 매 프레임 호출: 세 타이머 모두 내부 millis() 경과를 확인해
// 인터벌이 도래했으면 등록된 콜백을 실행한다.
// esp_task_wdt_reset()을 매 프레임 호출 — loop()가 여기까지 정상적으로 도달하고 있다는
// 뜻이므로 워치독 타이머를 리셋한다. 이 함수가 30초 넘게 안 불리면(=loop()가 어딘가에서
// 멈춤) 워치독이 패닉을 일으켜 자동 재부팅한다.
void TimerRun(){
    esp_task_wdt_reset();
    g_loop_count++;
    WifiTimer.run();
    GameTimer.run();

    BlinkTimer.run();
}
