void TimerInit(){
    wifiTimerId = WifiTimer.setInterval(2000,WifiIntervalFunc);
    // gameTimerId = GameTimer.setInterval(500,GameTimerFunc);
    // GameTimer.deleteTimer(gameTimerId);
    // subSerialTimerId = SubSerialTimer.setInterval(1000,SubSerialTimerFunc);
    // SubSerialTimer.deleteTimer(subSerialTimerId);
    // debuffTimerId = DebuffTimer.setInterval(1000,DebuffTimerFunc);
    // DebuffTimer.deleteTimer(debuffTimerId);
}
void TimerRun(){
    WifiTimer.run();
    GameTimer.run();
    SubSerialTimer.run();
    DebuffTimer.run();
    ApplyPendingDeviceState();
}

/**
 * @brief WIFI read 타이머 주기별로 받는 함수
 */
void WifiIntervalFunc(){
    has2wifi.Loop(DataChanged);
    CommnunicationBeetle();         // Sub Beetle
    CommnunicationMainBeetle();     // Main Beetle
}

/**
 * @brief 다중 태그를 인식하기 위한 타이머 함수
 */
void GameTimerFunc(){
    Serial.println("GameTimer");
    if (ptrGameTimer != nullptr) ptrGameTimer();
}

/**
 * @brief 반대쪽 SUB 태그머신(Beetle)에서 데이터를 타이머 주기별로 받는 함수
 */
void SubSerialTimerFunc(){
    SubSerialTimer.deleteTimer(subSerialTimerId);
    SubSerialTimerStart = false;
    if (ptrRfidFail != nullptr) ptrRfidFail();
    while(toSubSerial.available())
      toSubSerial.read();
    while(toMainSerial.available())
      toMainSerial.read();
}
void DebuffTimerFunc(){
    DebuffTimer.deleteTimer(debuffTimerId);
    Serial.println("debuff time end");
    has2wifi.Send((String)(const char*)my["device_name"], "device_state", "activate");
    ReturnNormalState();
}

void CancelTagProgress(){
    GameTimer.deleteTimer(gameTimerId);
    SubSerialTimer.deleteTimer(subSerialTimerId);
    SubSerialTimerStart = false;
    gameTimerCnt = 0;
    loginDone = false;

    ptrRfidMain = CommnunicationMainBeetle;
    ptrRfidSub = CommnunicationBeetle;
    ptrRfidMode = Login;
    ptrRfidFail = WaitFunc;

    WifiTimer.deleteTimer(wifiTimerId);                                          // WiFi 폴링 재시작 (ReturnNormalState와 동일)
    wifiTimerId = WifiTimer.setInterval(2000,WifiIntervalFunc);

    SubSerialFlush();
    MainSerialFlush();
    Serial.println("Cancel Tag Progress");
    ApplyPendingDeviceState();
}

/**
 * @brief 일반 상태로 돌아가는 함수
 */
void ReturnNormalState(){
    ptrRfidMain = CommnunicationMainBeetle;
    ptrRfidSub = CommnunicationBeetle;
    ptrRfidMode = Login;
    ptrRfidFail = WaitFunc;
    gameTimerCnt = 0;

    loginDone = false;
    GameTimer.deleteTimer(gameTimerId); 
    SubSerialTimer.deleteTimer(subSerialTimerId);   
    WifiTimer.deleteTimer(wifiTimerId);                                          //게임 타이머 종료
    wifiTimerId = WifiTimer.setInterval(2000,WifiIntervalFunc);
    
    Serial.println("Return Normal State");
    SubSerialFlush();                                                               //시리얼 통신 버퍼 flush
    MainSerialFlush();                                                              //Main Beetle 시리얼 버퍼 flush
}

/** 
 * @brief 잠기지 않은 도어를 플레이어가 도어가 잠금을 하기위한 함수
 */
void PlayerLockTimerFunc(){
    gameTimerCnt++;
    RoundNeoToggle(GREEN,gameTimerCnt);
    LineNeoUp(GREEN, YELLOW, map(gameTimerCnt,0,playerLockTime,0,NumPixels[LINE]));
    Serial.println(map(gameTimerCnt,0,playerLockTime,0,NumPixels[LINE]));
    // if(gameTimerCnt == 1)  // [DFPlayer 비활성화] 3번마다 "도어잠금 효과음"
    //     Mp3PlayLargeFolder(1, VD11);
    if(gameTimerCnt > (playerLockTime))
    {
        has2wifi.ReceiveMine();
        DataChanged();
        // Serial.println("strCurState:" + String(strCurState));
        if(strCurState != "activate"){
            Serial.println("debuff on");
            CancelTagProgress();
        }
        else {
            Serial.println("debuff off");
            has2wifi.Send((String)(const char*)my["device_name"], "device_state", "lock");
            Serial.println("DOOR LOCK!");
            // Mp3PlayLargeFolder(1, VD4);  // [DFPlayer 비활성화]
            has2wifi.ReceiveMine();
            ReturnNormalState();
            RoundNeoEffect(GREEN);
            SubSerialFlush();
            MainSerialFlush();
        }                                                     //시리얼 통신 버퍼 flush
    }
}

/**
 * @brief 잠겨있는 도어를 플레이어가 잠금해제를 하기위한 함수
 */
void PlayerUnlockTimerFunc(){       
    gameTimerCnt++;
    RoundNeoToggle(GREEN,gameTimerCnt);
    LineNeoDown(YELLOW, GREEN, map(gameTimerCnt,0,playerUnlockTime,0,NumPixels[LINE]));
    // if(gameTimerCnt == 1)  // [DFPlayer 비활성화] 3번마다 "도어잠금 효과음"
    //     Mp3PlayLargeFolder(1, VD11);
    if(gameTimerCnt > (playerUnlockTime))
    {
        has2wifi.ReceiveMine();
        DataChanged();
        // Serial.println("strCurState:" + String(strCurState));
        if(strCurState != "lock"){
            Serial.println("debuff on");
            CancelTagProgress();
        }
        else {
            Serial.println("DOOR UNLOCK!");
            // Mp3PlayLargeFolder(1, VD7);  // [DFPlayer 비활성화]
            ReturnNormalState();
            digitalWrite(RELAY_PIN, HIGH);
            has2wifi.Send((String)(const char*)my["device_name"], "device_state", "open");
            RoundNeoEffect(YELLOW);
            DoorOpen();
            has2wifi.ReceiveMine();
            SubSerialFlush();
            MainSerialFlush();
        }                                                       //시리얼 통신 버퍼 flush
    }
}

/**
 * @brief 잠겨있는 도어를 술래가 잠금해제를 하기위한 함수
 */
void TaggerUnlockTimerFunc(){
    gameTimerCnt++;
    RoundNeoToggle(PURPLE,gameTimerCnt);
    LineNeoDown(PURPLE, GREEN, map(gameTimerCnt,0,taggerUnlockTime,0,NumPixels[LINE]));
    // #6: NeoPixel show() 모두 끝낸 뒤 오디오(SoftwareSerial) 호출 → 인터럽트 충돌/데드락 회피
    // if(gameTimerCnt%3 == 1)  // [DFPlayer 비활성화] 3번마다 "술래 침입시도"
    //     if(gameTimerCnt < (taggerUnlockTime - 2))
    //         Mp3PlayLargeFolder(1, VD10);
    if(gameTimerCnt > (taggerUnlockTime))
    {
        has2wifi.ReceiveMine();
        DataChanged();
        // Serial.println("strCurState:" + String(strCurState));
        if(strCurState != "lock"){
            Serial.println("debuff on");
            CancelTagProgress();
        }
        else {
            // Mp3PlayLargeFolder(1, VD1);  // [DFPlayer 비활성화]
            Serial.println("DOOR UNLOCK!");
            ReturnNormalState();
            digitalWrite(RELAY_PIN, HIGH);
            has2wifi.Send((String)(const char*)my["device_name"], "device_state", "open");
            RoundNeoEffect(PURPLE);
            DoorOpen();
            SubSerialFlush();
            MainSerialFlush();
        }                                                          //시리얼 통신 버퍼 flush
    }
}

/**
 * @brief 잠겨있는 도어를 유령이 잠금해제를 하기위한 함수
 */
void GhostUnlockTimerFunc(){                                                
    gameTimerCnt++;
    // RoundNeoToggle(BLUE,gameTimerCnt);
    // LineNeoDown(BLUE, GREEN, map(gameTimerCnt,0,ghostOpenTime,0,NumPixels[LINE]));
    RoundNeoUp(BLUE, GREEN, map(gameTimerCnt,0,ghostOpenTime,0,NumPixels[ROUND]/2));
    if(gameTimerCnt > (ghostOpenTime))
    {
        has2wifi.ReceiveMine();
        DataChanged();
        // Serial.println("strCurState:" + String(strCurState));
        if(strCurState != "lock"){
            Serial.println("debuff on");
            CancelTagProgress();
        }
        else{
            // Mp3PlayLargeFolder(1, VD1);  // [DFPlayer 비활성화]
            Serial.println("GHOST OPEN");
            ReturnNormalState();
            digitalWrite(RELAY_PIN, HIGH);
            has2wifi.Send((String)(const char*)my["device_name"], "device_state", "open");
            RoundNeoEffect(BLUE);
            GhostDoorOpen();
            if(TaggerOverrideCheck()) return; // 연출 중 tagger 수신 시 lock으로 덮어쓰지 않음
            has2wifi.Send((String)(const char*)my["device_name"], "device_state", "lock");
            AllNeoOn(GREEN);
            SubSerialFlush();
            MainSerialFlush();
            delay(1000);
            has2wifi.Loop(DataChanged); //LOCK -> ACTIVATE 바뀐것을 업데이트 받기 위함
        }
    }

}

/**
 * @brief 뉴비모드: 술래가 잠금해제 후 lock 상태로 복귀
 */
void NewbieTaggerUnlockTimerFunc(){
    gameTimerCnt++;
    RoundNeoToggle(PURPLE, gameTimerCnt);
    LineNeoDown(PURPLE, GREEN, map(gameTimerCnt, 0, taggerUnlockTime, 0, NumPixels[LINE]));
    // #6: NeoPixel show() 모두 끝낸 뒤 오디오(SoftwareSerial) 호출 → 인터럽트 충돌/데드락 회피
    // if(gameTimerCnt%3 == 1)  // [DFPlayer 비활성화]
    //     if(gameTimerCnt < (taggerUnlockTime - 2))
    //         Mp3PlayLargeFolder(1, VD10);
    if(gameTimerCnt > (taggerUnlockTime))
    {
        has2wifi.ReceiveMine();
        DataChanged();
        if(strCurState != "lock"){
            Serial.println("debuff on");
            CancelTagProgress();
        }
        else {
            // Mp3PlayLargeFolder(1, VD1);  // [DFPlayer 비활성화]
            Serial.println("DOOR UNLOCK (Newbie)!");
            ReturnNormalState();
            ptrRfidMode = NewbieLogin;
            digitalWrite(RELAY_PIN, HIGH);
            has2wifi.Send((String)(const char*)my["device_name"], "device_state", "open");
            RoundNeoEffect(PURPLE);
            GhostDoorOpen();
            if(TaggerOverrideCheck()) return; // 연출 중 tagger 수신 시 lock으로 덮어쓰지 않음
            has2wifi.Send((String)(const char*)my["device_name"], "device_state", "lock");
            AllNeoOn(GREEN);
            SubSerialFlush();
            MainSerialFlush();
            delay(1000);
            has2wifi.Loop(DataChanged);
        }
    }
}

/**
 * @brief 잠겨있지 않은 도어 유령이 잠금해제를 하기위한 함수
 */
void GhostLockTimerFunc(){      
    gameTimerCnt++;
    // RoundNeoToggle(BLUE,gameTimerCnt);
    // LineNeoUp(BLUE, YELLOW, map(gameTimerCnt,0,ghostOpenTime,0,NumPixels[LINE]));
    RoundNeoUp(BLUE, YELLOW, map(gameTimerCnt,0,ghostOpenTime,0,NumPixels[ROUND]/2));
    if(gameTimerCnt > (ghostOpenTime))
    {
        has2wifi.ReceiveMine();
        DataChanged();
        // Serial.println("strCurState:" + String(strCurState));
        if(strCurState != "activate"){
            Serial.println("debuff on");
            CancelTagProgress();
        }
        else {
            // Mp3PlayLargeFolder(1, VD1);  // [DFPlayer 비활성화]
            Serial.println("GHOST OPEN");
            ReturnNormalState();
            digitalWrite(RELAY_PIN, HIGH);
            has2wifi.Send((String)(const char*)my["device_name"], "device_state", "open");
            RoundNeoEffect(BLUE);
            GhostDoorOpen();
            if(TaggerOverrideCheck()) return; // 연출 중 tagger 수신 시 activate로 덮어쓰지 않음
            has2wifi.Send((String)(const char*)my["device_name"], "device_state", "activate");
            AllNeoOn(YELLOW);
            SubSerialFlush();
            MainSerialFlush();
            delay(1000);
            has2wifi.Loop(DataChanged); //LOCK -> ACTIVATE 바뀐것을 업데이트 받기 위함
        }
    }
}
