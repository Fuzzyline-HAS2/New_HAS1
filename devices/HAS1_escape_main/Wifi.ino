void DataChanged()
{
  String myJson;
  serializeJson(my, myJson);
  Serial.println(myJson);
  static StaticJsonDocument<2048> cur;
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
