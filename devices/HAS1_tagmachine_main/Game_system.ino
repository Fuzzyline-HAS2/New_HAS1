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
    // Send()는 서버 전송만 하고 로컬 my를 갱신하지 않으며, 뒤이은 Loop(DataChanged)도
    // shift_machine 플래그가 안 서면 my를 안 갱신한다. 그대로 두면 TaggerOverrideCheck의
    // ReceiveMine()이 방금 보낸 "open"을 로컬 my에 남긴 채로 굳어버려, 다음 태그 판정
    // (LoginTimerSelector)이 "lock"이 아닌 것으로 보고 일반 모드의 즉시 오픈 분기로 새어
    // 타이머 없이 바로 문이 열리고 activate로 전송되는 버그가 난다.
    my["device_state"] = "lock";
    strCurState = "lock";
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
    my["device_state"] = "lock";  // NewbiePlayerOpen 참고: Send()는 로컬 my를 안 갱신하므로 직접 동기화
    strCurState = "lock";
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