void DataChanged()
{
  String myJson;
  serializeJson(my, myJson);
  Serial.println(myJson);

  // 한 번의 폴링이 game_state와 device_state를 동시에 옮겨오면 아래 두 블록이 모두
  // 조건을 만족해 같은 동작을 두 번 실행한다. 실측 2026-09-13: 대시보드에서 activate로
  // 바꾸자 41.615s에 문이 열리고(4초) 3ms 뒤 45.623s에 또 열렸다(4초) — 총 8초.
  // cur 갱신은 함수 끝에서 한 번에 하므로 두 블록 모두 갱신 전의 cur을 본다.
  // 상태 적용은 이 호출당 한 번으로 제한한다.
  bool stateApplied = false;
  if((String)(const char*)my["game_state"] != (String)(const char*)cur["game_state"]){
    if((String)(const char*)my["game_state"] == "setting"){
        SettingFunc();
        stateApplied = true;
    }
    else if((String)(const char*)my["game_state"] == "ready"){
        ReadyFunc();
        stateApplied = true;
    }
    else if((String)(const char*)my["game_state"] == "activate"){
        if((String)(const char*)my["device_state"] != "fake" && (String)(const char*)my["device_state"] != "tagger"){
            ActivateFunc();
            stateApplied = true;
        }
    }
    else if((String)(const char*)my["game_state"] == "escape"){
        EscapeClose();
        GameTimer.disable(gameTimerId);
        ptrCurrentMode = WaitFunc;
    }
  }
  if (my["brightness"].as<int>() != cur["brightness"].as<int>())
    UpdateBrightness();
  if((String)(const char*)my["device_state"] != (String)(const char*)cur["device_state"]){
    if((String)(const char*)my["device_state"] == "player_win"){
        AllNeoOn(BLUE);
        EscapeClose();
    }
    else if((String)(const char*)my["device_state"] == "fake"){
        AllNeoOn(PURPLE);
        EscapeClose();
    }
    else if((String)(const char*)my["device_state"] == "tagger"){
        AllNeoOn(PURPLE);
        EscapeClose();
    }
    else if((String)(const char*)my["device_state"] == "activate"){
        // game_state 블록에서 이미 열었으면 다시 열지 않는다.
        if((String)(const char*)my["game_state"] == "activate" && !stateApplied){
            ActivateFunc();
        }
    }
    else if((String)(const char*)my["device_state"] == "github"){
        Serial.println("[OTA] OTA 업데이트 요청 수신");
        ota.check();
    }
  }
  cur = my;
}

void WaitFunc(){
}

void SettingFunc(void)
{
    Serial.println("SETTING");
    digitalWrite(RELAY_PIN, HIGH);
    AllNeoOn(WHITE);
    EscapeClose();
    ptrCurrentMode = WaitFunc;
    GameTimer.disable(gameTimerId);
}

void ActivateFunc(void){
    Serial.println("ACTIVATE");
    Mp3PlayLargeFolder(1, VE1);
    AllNeoOn(YELLOW);
    EscapeOpen();
    GameTimer.enable(gameTimerId);
    toSubSerial.flush();
    ptrCurrentMode = TagCount;
}

void ReadyFunc(void){
    Serial.println("READY");
    digitalWrite(RELAY_PIN, HIGH);
    AllNeoOn(RED);
    EscapeClose();
    ptrCurrentMode = WaitFunc;
}
