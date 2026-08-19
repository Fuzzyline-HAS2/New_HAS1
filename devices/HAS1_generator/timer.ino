// =================================================================================
// timer.ino
// ---------------------------------------------------------------------------------
// SimpleTimer 기반 3개 인터벌(WifiTimer/GameTimer/BlinkTimer)의 초기화와 콜백을 모아놓은 파일.
// TimerRun()이 매 loop마다 세 타이머를 갱신(run)하고, 각 인터벌이 도래하면 아래 콜백들이 호출된다.
// GameTimer/BlinkTimer는 상시 동작하는 게 아니라 필요할 때만 deleteTimer/setInterval로 켜고 끈다.
// =================================================================================

// setup()에서 1회 호출: 세 인터벌을 등록한 뒤, GameTimer/BlinkTimer는 곧바로 정지시킨다
// (WifiTimer만 상시 폴링, 나머지 둘은 게임 진행 상황에 따라 필요할 때 다시 켜짐).
void TimerInit(){
    wifiTimerId = WifiTimer.setInterval(wifiTime,WifiIntervalFunc);
    gameTimerId = GameTimer.setInterval(gameTime,GameTimerFunc);

    blinkTimerId = BlinkTimer.setInterval(blinkTime,BlinkTimerFunc);

    GameTimer.deleteTimer(gameTimerId);

    BlinkTimer.deleteTimer(blinkTimerId);
}

// WifiTimer 콜백 (wifiTime = 2000ms마다 실행): 서버와 통신해 my/tag 등의 JSON을 갱신하고,
// 변경분이 있으면 DataChanged() 콜백을 통해 게임 상태 전환을 처리한다.
void WifiIntervalFunc(){
    has2wifi.Loop(DataChanged);
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
void TimerRun(){
    WifiTimer.run();
    GameTimer.run();

    BlinkTimer.run();
}
