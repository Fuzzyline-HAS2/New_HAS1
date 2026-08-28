void CheckingPlayers(String tagUser) // 어떤 카드가 들어왔는지 확인용
{
  Serial.println("tag_user_data : " +
                 tagUser); // 1. 태그한 플레이어의 역할과 생명칩갯수,
                           // 최대생명칩갯수 등 읽어오기
  if (tagUser == "MMMM") { // 스태프카드
    digitalWrite(RELAY_PIN, HIGH);
    delay(500);
    digitalWrite(RELAY_PIN, LOW);
  } else {
    if (loginDone == false) // 로그인할때만 체크함
    {
      has2wifi.Receive(tagUser); // 2. 술래인지, 플레이어인지 구분
      if ((String)(const char *)tag["role"] ==
          "player") { // 3. 태그한 사용자가 플레이어고
        Serial.println("Player Tagged");
        loginRole = 'P';
        if (ptrRfidMode != nullptr) ptrRfidMode('P');
      } else if ((String)(const char *)tag["role"] ==
                 "tagger") { // 3. 태그한 사용자가 플레이어고
        Serial.println("Tagger Tagged");
        loginRole = 'T';
        if (ptrRfidMode != nullptr) ptrRfidMode('T');
      } else if ((String)(const char *)tag["role"] == "ghost" ||
                 (String)(const char *)tag["role"] == "revival") { // 3. 태그한
        Serial.println("Ghost Tagged");
        loginRole = 'G';
        if (ptrRfidMode != nullptr) ptrRfidMode('G');
      } else {
        Serial.println("Wrong TAG");
      }
    } else {
      if (strLastTagUser == tagUser) { // Login 진행한 태그와 같은지 확인
        tagMismatchCount = 0;          // 정상 읽기 → 불일치 카운터 리셋
        Serial.println("LoginRole: " + String(loginRole));
        if (ptrRfidMode != nullptr) ptrRfidMode(loginRole);
      } else {
        // 진행 중 단발성 불일치는 시리얼 손상(예: G1P4->1P4)일 가능성이 높으므로 무시.
        // 3회 연속 불일치일 때만 실제 다른 카드로 보고 fail 처리.
        tagMismatchCount++;
        if (tagMismatchCount >= 3) {
          Serial.println("Different TAG deteced (x3)");
          tagMismatchCount = 0;
          if (ptrRfidFail != nullptr) ptrRfidFail();
        } else {
          Serial.println("Mismatch ignored (transient)");
        }
      }
    }
    strLastTagUser = tagUser;
  }
}

void Login(char role) {
  Serial.println("LOGIN");
  loginDone = true;
  lightColor(pixels[ROUND], color[BLACK]);
  lightColor(pixels[ROUND_SUB], color[BLACK]);

  GameTimer.deleteTimer(gameTimerId);
  gameTimerId = GameTimer.setInterval(1000, GameTimerFunc);
  WifiTimer.deleteTimer(wifiTimerId);
  gameTimerCnt = 0;
  tagMismatchCount = 0;   // 새 로그인 시작 시 불일치 카운터 초기화
  if (mainRfidTagged == true) {
    ptrRfidMain = CommnunicationMainBeetle;
    ptrRfidSub = WaitFunc;
    LoginTimerSelector(role);
  } else if (mainRfidTagged == false) {
    ptrRfidMain = WaitFunc;
    ptrRfidSub = CommnunicationBeetle;
    LoginTimerSelector(role);
  }
  if (loginDone && ptrGameTimer != nullptr) ptrGameTimer();
}

void WaitRfid(char role) { Serial.println("WAIT RFID"); }
void LoginTimerSelector(char role) {
  Serial.println("LoginTimerSelector");
  if ((String)(const char *)my["device_state"] == "lock") {
    if (role == 'P') {
      ptrGameTimer = PlayerUnlockTimerFunc;
      ptrRfidFail = UnlockFail;
      ptrRfidMode = WaitRfid;
    } else if (role == 'G') {
      ptrGameTimer = GhostUnlockTimerFunc;
      ptrRfidFail = GhostOpenFailLock;
      ptrRfidMode = WaitRfid;
    } else if (role == 'T') {
      ptrGameTimer = TaggerUnlockTimerFunc;
      ptrRfidFail = UnlockFail;
      ptrRfidMode = WaitRfid;
    }
  } else if ((String)(const char *)my["device_state"] == "debuff") {
    AllNeoOn(PURPLE);
    if (role == 'P') {
      NeoBlink(ROUND, RED, 2, 400);
      AllNeoOn(PURPLE);
      ReturnNormalState();
    } else if (role == 'G') {
      NeoBlink(ROUND, RED, 2, 400);
      AllNeoOn(PURPLE);
      ReturnNormalState();
    } else if (role == 'T') {
      // Mp3PlayLargeFolder(1, VD1);  // [DFPlayer 비활성화]
      Serial.println("Tagger Door Open");
      digitalWrite(RELAY_PIN, HIGH);

      RoundNeoEffect(PURPLE);
      AllNeoOn(PURPLE);
      RoundNeoEffectDown(BLACK);
      DoorOpen();
      ReturnNormalState(); // 시리얼 통신 버퍼 flush
      AllNeoOn(PURPLE);
    }
  } else if ((String)(const char *)my["device_state"] == "tagger") {
    if (role == 'T') {
      // 술래 카드: 아무 동작 없음 (Login()이 지운 ROUND만 보라색으로 복귀)
      AllNeoOn(PURPLE);
      ReturnNormalState();
    } else {                          // 'P' 또는 'G' = 술래 외
      NeoBlink(ROUND, RED, 3, 500);   // 적색 점멸 3초
      AllNeoOn(PURPLE);               // 보라색 복귀
      ReturnNormalState();
    }
  } else // 도어 UNLOCK 일때
  {
    if (role == 'P') {
      Serial.println("LoginTimerSelector PlayerSelected");
      ptrGameTimer = PlayerLockTimerFunc;
      ptrRfidFail = LockFail;
      ptrRfidMode = WaitRfid;
    } else if (role == 'G') {
      ptrGameTimer = GhostLockTimerFunc;
      ptrRfidFail = GhostOpenFailUnlock;
      ptrRfidMode = WaitRfid;
    } else if (role == 'T') {
      // Mp3PlayLargeFolder(1, VD1);  // [DFPlayer 비활성화]
      Serial.println("Tagger Door Open");
      digitalWrite(RELAY_PIN, HIGH);
      has2wifi.Send((String)(const char *)my["device_name"], "device_state",
                    "open");
      RoundNeoEffect(PURPLE);
      AllNeoOn(PURPLE);
      DoorOpen();
      if (strCurState != "tagger") // DoorOpen 중 tagger 적용 시 노랑으로 덮어쓰지 않음
        AllNeoOn(YELLOW);
      ReturnNormalState();
    }
  }
  if ((String)(const char*)my["mode"] == "easy" &&
      (String)(const char*)my["device_state"] == "lock") {
    if (role == 'P' || role == 'G') NewbieLogin(role);
    else if (role == 'T') {
      ptrGameTimer = NewbieTaggerUnlockTimerFunc;
      ptrRfidFail = NewbieTaggerFail;
    }
  }
}
void NewbieTaggerFail() {
  has2wifi.ReceiveMine();
  DataChanged();
  if (strCurState != "lock") {
    Serial.println("debuff on");
    CancelTagProgress();
  } else {
    // Mp3PlayLargeFolder(1, VD6);  // [DFPlayer 비활성화]
    Serial.println("Unlock Fail Door Shut");
    NeoBlink(ROUND, RED, 2, 150);   // 5,500(5초 블로킹) -> 2,150(~0.6초): loop freeze 방지
    AllNeoOn(GREEN);
    ReturnNormalState();
    ptrRfidMode = NewbieLogin;
  }
}
void LockFail() {
  has2wifi.ReceiveMine();
  DataChanged();
  // Serial.println("strCurState:" + String(strCurState));
  if (strCurState != "activate") {
    Serial.println("debuff on");
    CancelTagProgress();
  } else {
    // Mp3PlayLargeFolder(1, VD1);  // [DFPlayer 비활성화]
    Serial.println("Lock Fail Door Open");
    digitalWrite(RELAY_PIN, HIGH);
    has2wifi.Send((String)(const char *)my["device_name"], "device_state",
                  "open");
    RoundNeoEffect(YELLOW);
    AllNeoOn(YELLOW);
    DoorOpen();
    ReturnNormalState();
  }
}
void UnlockFail() {
  has2wifi.ReceiveMine();
  DataChanged();
  // Serial.println("strCurState:" + String(strCurState));
  if (strCurState != "lock") {
    Serial.println("debuff on");
    CancelTagProgress();
  } else {
    // Mp3PlayLargeFolder(1, VD6);  // [DFPlayer 비활성화]
    Serial.println("Unlock Fail Door Shut");
    NeoBlink(ROUND, RED, 2, 150);   // 5,500(5초 블로킹) -> 2,150(~0.6초): loop freeze 방지
    AllNeoOn(GREEN);
    ReturnNormalState();
  }
}

void GhostOpenFailUnlock() {
  // has2wifi.ReceiveMine();
  // DataChanged();
  // // Serial.println("strCurState:" + String(strCurState));
  // if(strCurState != "lock"){
  //     Serial.println("debuff on");
  // }
  // else{
  // Mp3PlayLargeFolder(1, VD6);  // [DFPlayer 비활성화]
  Serial.println("Ghost Door OpenFail");
  NeoBlink(ROUND, RED, 2, 150);   // 5,500(5초 블로킹) -> 2,150(~0.6초): loop freeze 방지
  AllNeoOn(YELLOW);
  ReturnNormalState();
  // // }
}
void GhostOpenFailLock() {
  // has2wifi.ReceiveMine();
  // DataChanged();
  // // Serial.println("strCurState:" + String(strCurState));
  // if(strCurState != "activate"){
  //     Serial.println("debuff on");
  // }
  // else{
  // Mp3PlayLargeFolder(1, VD6);  // [DFPlayer 비활성화]
  Serial.println("Unlock Fail Door Shut");
  NeoBlink(ROUND, RED, 2, 150);   // 5,500(5초 블로킹) -> 2,150(~0.6초): loop freeze 방지
  AllNeoOn(GREEN);
  ReturnNormalState();
  // }
}
