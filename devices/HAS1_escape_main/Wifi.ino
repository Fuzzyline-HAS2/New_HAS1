void DataChanged()
{
  String myJson;
  serializeJson(my, myJson);
  Serial.println(myJson);
  // JsonDocument(크기 템플릿 없는 v7 타입) 사용 — StaticJsonDocument<N>은 N이 my와 정확히
  // 같아야만 대입(operator=)이 되는데, 로컬/CI에 깔린 HAS2_Wifi 사본마다 my의 선언 크기가
  // 다를 수 있어(예: 1000 vs 2048) 매번 컴파일 에러가 났다(HAS1_itembox와 동일 이슈).
  // JsonDocument는 크기에 상관없이 대입/set()이 되므로 어떤 환경에서도 안전하다.
  static JsonDocument cur;
  if((String)(const char*)my["game_state"] != (String)(const char*)cur["game_state"]){
    if((String)(const char*)my["game_state"] == "setting"){
        SettingFunc();
    }
    else if((String)(const char*)my["game_state"] == "ready"){
        ReadyFunc();
    }
    else if((String)(const char*)my["game_state"] == "activate"){
        if((String)(const char*)my["device_state"] != "fake"){
            ActivateFunc();
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
    else if((String)(const char*)my["device_state"] == "activate"){
        if((String)(const char*)my["game_state"] == "activate"){
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
