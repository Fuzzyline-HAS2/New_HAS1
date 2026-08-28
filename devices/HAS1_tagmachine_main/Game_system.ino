void WhichTagged()
{
    if (ptrRfidMain != nullptr) ptrRfidMain();
    if (ptrRfidSub  != nullptr) ptrRfidSub();
}

void DoorOpen(){
    digitalWrite(RELAY_PIN, LOW);
    if(strCurState == "debuff"){  //debuff 인경우위해서?
        Serial.println("DEBUFF OPEN");
    }
    else{
        if(TaggerOverrideCheck()) return; // 전송 직전 tagger 수신 시 activate로 덮어쓰지 않음
        has2wifi.Send((String)(const char*)my["device_name"], "device_state", "activate");
        RoundNeoEffectDown(BLACK);
        has2wifi.Loop(DataChanged); //LOCK -> ACTIVATE 바뀐것을 업데이트 받기 위함
        if(strCurState == "tagger") return; // Loop에서 tagger 적용 시 노랑/도어잠금으로 덮어쓰지 않음
        AllNeoOn(YELLOW);
    }
    digitalWrite(RELAY_PIN, LOW);
}

void GhostDoorOpen(){
    digitalWrite(RELAY_PIN, LOW);
    RoundNeoEffectDown(BLACK);
    // delay(3000);
}

// ======================== NEWBIE MODE ========================

void NewbiePlayerOpen() {
    ReturnNormalState();
    ptrRfidMode = NewbieLogin;
    // Mp3PlayLargeFolder(1, VD1);  // [DFPlayer 비활성화]
    digitalWrite(RELAY_PIN, HIGH);
    has2wifi.Send((String)(const char*)my["device_name"], "device_state", "open");
    RoundNeoEffect(GREEN);
    GhostDoorOpen();
    if(TaggerOverrideCheck()) return; // 연출 중 tagger 수신 시 lock으로 덮어쓰지 않음
    has2wifi.Send((String)(const char*)my["device_name"], "device_state", "lock");
    AllNeoOn(GREEN);
    SubSerialFlush();
    MainSerialFlush();
    delay(1000);
    has2wifi.Loop(DataChanged);
}

void NewbieGhostOpen() {
    ReturnNormalState();
    ptrRfidMode = NewbieLogin;
    // Mp3PlayLargeFolder(1, VD1);  // [DFPlayer 비활성화]
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
    has2wifi.Loop(DataChanged);
}

void NewbieLogin(char role) {
    if (role == 'T') {
        Login(role);
    } else if (role == 'P') {
        NewbiePlayerOpen();
    } else if (role == 'G') {
        NewbieGhostOpen();
    }
}