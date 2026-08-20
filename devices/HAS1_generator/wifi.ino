// =================================================================================
// wifi.ino
// ---------------------------------------------------------------------------------
// 서버와의 통신 결과(my: 이 기기 상태 JSON, tag: 마지막으로 조회한 태그 정보 JSON)를 받아
// 게임 상태 머신을 전환하는 핵심 로직. WifiIntervalFunc()가 주기적으로 has2wifi.Loop(DataChanged)를
// 호출하고, 서버 응답이 오면 DataChanged()가 콜백으로 실행된다.
//
// 상태는 크게 두 축으로 나뉜다.
//   - my["game_state"]   : 방 전체의 큰 흐름 (setting / ready / activate)
//   - my["device_state"] : 이 기기(발전기) 개별 진행 상태
//                          (repaired / repaired_all / battery_max / starter_finish /
//                           player_win / player_lose / github(OTA 트리거) 등)
// =================================================================================

// WifiTimer에 의해 주기적으로 호출됨 (has2wifi.Loop 콜백).
// 직전에 저장해둔 값(cur)과 새로 받아온 값(my)을 비교해서, 바뀐 항목에 해당하는 처리만 수행한다.
// 이런 "diff 후 처리" 방식 덕분에 서버 폴링이 반복되어도 같은 동작이 중복 실행되지 않는다.
void DataChanged()
{
  // JsonDocument(크기 템플릿 없는 v7 타입) 사용 — StaticJsonDocument<N>은 N이 my와 정확히
  // 같아야만 대입(operator=)이 되는데, 로컬/CI에 깔린 HAS2_Wifi 사본마다 my의 선언 크기가
  // 다를 수 있어(예: 1000 vs 2048) 매번 컴파일 에러가 났다. JsonDocument는 크기에 상관없이
  // 대입/set()이 되므로 어떤 환경에서도 안전하다.
  static JsonDocument cur;  //저장되어 있는 cur과 읽어온 my 값과 비교후 실행

  // 서버에서 받은 스타터 설정값 동기화 (0 이하인 값은 아직 세팅 전이라 판단해 무시)
  if((int)my["starter_encoder_unit"] > 0)  starterEncoderUnit  = (int)my["starter_encoder_unit"];
  if((int)my["starter_decrease_amount"] > 0) starterDecreaseAmount = (int)my["starter_decrease_amount"];

  // 밝기 값이 바뀌었으면 즉시 LED에 반영
  if(my["brightness"].as<int>() != cur["brightness"].as<int>()) {
    UpdateBrightness();
  }

  // ---- game_state(방 전체 진행 단계) 변화 처리 ----
  bool gameStateChanged = (String)(const char*)my["game_state"] != (String)(const char*)cur["game_state"];
  if(gameStateChanged){
    if((String)(const char*)my["game_state"] == "setting"){
      SettingFunc();
    }
    else if((String)(const char*)my["game_state"] == "ready"){
      ReadyFunc();
    }
    else if((String)(const char*)my["game_state"] == "activate"){
      ActivateFunc();
    }
  }

  // 이미 "repaired" 상태에서 남은 발전기 개수(left_generator)만 갱신된 경우 안내 음원만 재생
  if((String)(const char*)my["left_generator"] != (String)(const char*)cur["left_generator"]){
    if((String)(const char*)my["device_state"] == "repaired"){
      LeftGenerator();
    }
  }

  // receiveMineOn이 true인 동안은(StartFinish 등에서 직접 상태를 전환 중) 아래 device_state 분기를
  // 건너뛴다 — 서버 폴링 결과가 방금 로컬에서 결정한 상태를 덮어쓰지 않도록 하는 가드.
  if(receiveMineOn == false){
    // battery_pack 값 변화(activate 상태에서 배선 충전 중일 때) — 배선 방식(WirePollMain)이 도입되기 전의
    // 경로로, 현재는 서버 쪽에서 battery_pack이 바뀌는 다른 경로가 있을 경우를 대비한 안전망 성격이 크다.
    if(gameStateChanged == false && cur.containsKey("battery_pack") && (String)(const char*)my["game_state"] == "activate" && (int)my["battery_pack"] != (int)cur["battery_pack"]){
      BatteryPackSend();
      if((int)my["battery_pack"] == (int)my["max_battery_pack"]){
        receiveMineOn = true;
        BatteryFinish();
      }
    }

    // ---- device_state(이 기기의 개별 상태) 변화 처리 ----
    if(receiveMineOn == false && (String)(const char*)my["device_state"] != (String)(const char*)cur["device_state"]){
      if((String)(const char*)my["device_state"] == "repaired_all"){
        // 모든 발전기가 수리 완료 — 더 이상 할 일 없음, 탈출구 오픈 안내
        ptrRfidMode = WaitFunc;
        ptrCurrentMode = WaitFunc;
        Mp3PlayLargeFolderAndWait(1, 6, 8000);  // device_state == "repaired_all"
        Mp3PlayLargeFolder(1, 2);               // device_state == "repaired_all"
        GameTimer.deleteTimer(gameTimerId);

        BlinkTimer.deleteTimer(blinkTimerId);
        AllNeoOn(BLUE);
      }
      else if((String)(const char*)my["device_state"] == "repaired"){
        // 서버 쪽에서 이미 repaired로 바뀐 걸 뒤늦게 수신한 경우 — rfid.ino의 StartFinish()와
        // 동일한 마무리 처리를 반복해 로컬 상태를 서버와 일치시킨다.
        Serial.println("StartFinish PTRFUNC");

        GameTimer.deleteTimer(gameTimerId);        //게임 타이머 종료
        BlinkTimer.deleteTimer(blinkTimerId);
        Serial.println("Generator Fixed!");
        Mp3PlayLargeFolderAndWait(1, 4, 8000);  // device_state == "repaired"
        LeftGenerator();
        AllNeoOn(BLUE);
        ptrCurrentMode = WaitFunc;
      }
      else if((String)(const char*)my["device_state"] == "battery_max"){
        // 서버가 battery_pack을 최대치로 리셋(다음 라운드 준비 등)한 경우 —
        // 로컬 배선 카운트와의 차이를 서버에 보정 전송한 뒤 ActivateFunc으로 재진입한다.
        int maxBattery = (int)my["max_battery_pack"] - (int)my["battery_pack"];
        Serial.println((String)maxBattery);
        has2wifi.Send((String)(const char*)my["device_name"], "battery_pack", ((String)maxBattery));

        GameTimer.deleteTimer(gameTimerId);        //게임 타이머 종료
        ActivateFunc();
      }
      else if((String)(const char*)my["device_state"] == "starter_finish"){
        // (별도 처리 없음 — 상태 값만 존재, 실제 전환은 ActivateFunc 진입 시 device_state 검사로 처리됨)
      }
      else if((String)(const char*)my["device_state"] == "player_win"){
        ptrRfidMode = WaitFunc;
        ptrCurrentMode = WaitFunc;
        AllNeoOn(BLUE);
        Mp3PlayLargeFolder(1, 1);  // TODO: PG_PLAYER_WIN 음원 지정
      }
      else if((String)(const char*)my["device_state"] == "player_lose"){
        ptrRfidMode = WaitFunc;
        ptrCurrentMode = WaitFunc;
        AllNeoOn(RED);
        Mp3PlayLargeFolder(1, 1);  // TODO: PG_PLAYER_LOSE 음원 지정
      }
      else if((String)(const char*)my["device_state"] == "github"){
        // 서버가 원격으로 OTA 업데이트를 트리거하는 채널
        Serial.println("[OTA] OTA 업데이트 요청 수신");
        ota.check();
      }
    }
  }
  else{
    // 이번 호출에서 receiveMineOn 가드를 소비했으니 다음 폴링부터는 다시 정상적으로 처리하도록 해제
    receiveMineOn = false;
  }

  cur = my; // cur 데이터 그룹에 현재 읽어온 데이터 저장 (다음 비교의 기준값이 됨)
}

// 아무 동작도 하지 않는 대기 콜백. ptrCurrentMode / ptrRfidMode의 기본값이자,
// 게임 진행이 필요 없는 상태(대기/완료 후)에 세팅되는 no-op 함수.
void WaitFunc(){

}

// game_state == "setting" 진입 시 호출 — 다음 라운드를 준비하며 모든 진행 상태를 초기값으로 되돌린다.
void SettingFunc(void){
    Serial.println("SETTING");
    Mp3PlayLargeFolder(1, 1);  // TODO: PG_PRE_TAGGER 음원 지정
    AllNeoOn(WHITE);
    encoderValue = 100;
    EncoderDetach();
    GameTimer.deleteTimer(gameTimerId);

    BlinkTimer.deleteTimer(blinkTimerId);
    ptrRfidMode = WaitFunc;
    ptrCurrentMode = WaitFunc;
    receiveMineOn = false;
}

// game_state == "activate" 진입 시 호출 — 라운드가 실제로 시작되는 시점.
// device_state에 따라 세 가지 경로 중 하나로 분기한다:
//   1) "starter_finish": 배터리 충전은 이미 끝났고 스타터(RFID+엔코더) 단계로 바로 진입
//   2) "battery_max"   : 배터리팩이 이미 가득 찬 상태로 시작 — BatteryFinish()로 스타터 진입
//   3) 그 외(기본)      : 배선 충전 단계부터 시작 — WirePollMain을 메인 루프로 등록
void ActivateFunc(void){
    Serial.println("ACTIVATE");
    AllNeoOn(YELLOW);
    BatteryPackSend();
    EncoderDetach();
    GameTimer.deleteTimer(gameTimerId);
    BlinkTimer.deleteTimer(blinkTimerId);
    nfc[MAINPN532].SAMConfig(); // RFID 리더 재설정 (직전 상태에서 통신이 꼬였을 경우 대비)
    if((String)(const char*)my["device_state"] == "starter_finish"){
        AllNeoOn(GREEN);
        ptrRfidMode = StartFinish;
        ptrCurrentMode = RfidLoopMain;
        NeoLightColor(GAUGE, color[BLUE]);
        BlinkTimer.deleteTimer(blinkTimerId);
        BlinkTimerStart(CIRCUIT, YELLOW); // 진행 중임을 알리기 위해 CIRCUIT 스트립을 노란색으로 점멸
    }
    else if((String)(const char*)my["device_state"] == "battery_max"){
        BatteryFinish();
    }
    else{
        // device_state == "activate" (배선 충전 시작 전 기본 상태)
        Mp3PlayLargeFolder(1, 1);
        // 배터리팩 충전은 더 이상 RFID 태그가 아니라 물리 배선 4개(WIRE_PIN_1~4)로 실시간 반영됨
        WireResetTracking();
        ptrCurrentMode = WirePollMain;
        if((int)my["battery_pack"] == (int)my["max_battery_pack"]){
            BatteryFinish();
        }
    }
}

// game_state == "ready" 진입 시 호출 — 라운드 시작 직전 대기 상태. 모든 진행을 멈추고 빨간 LED로 표시한다.
void ReadyFunc(void){
    Serial.println("READY");
    AllNeoOn(RED);
    Mp3PlayLargeFolder(1, 1);  // TODO: PG_PRE_TAGGER 음원 지정
    EncoderDetach();
    GameTimer.deleteTimer(gameTimerId);

    BlinkTimer.deleteTimer(blinkTimerId);
    ptrRfidMode = WaitFunc;
    ptrCurrentMode = WaitFunc;

}
