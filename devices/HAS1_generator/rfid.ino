// =================================================================================
// rfid.ino
// ---------------------------------------------------------------------------------
// PN532 RFID 리더 초기화, 태그 인식 루프, 태그 판독 후 역할(role) 분기를 담당하는 파일.
// RfidLoopMain()은 배터리팩 충전 전 단계(설정/준비 상태)에서 ptrCurrentMode로 등록되어
// 플레이어가 카드를 태그하면 CheckingPlayers()를 거쳐 ptrRfidMode()를 호출한다.
// (스타터 단계에서의 태그 판정은 이 파일이 아니라 Game_system.ino의 StarterActivate() 내부에
//  별도 로직으로 들어있다.)
// =================================================================================

// setup()에서 1회 호출: PN532와 통신을 열고 SAM(Secure Access Module) 설정을 적용한다.
// 초기화에 실패하면 전체 네오픽셀을 빨간색으로 켜서 하드웨어 문제를 시각적으로 알린다
// (goto 재시도 루틴은 주석 처리되어 있어 현재는 1회 시도 후 실패 상태로 넘어감).
void RfidInit()
{
  RestartPn532:
  nfc[MAINPN532].begin();
  if (!(nfc[MAINPN532].getFirmwareVersion()))
  {
    Serial.println("PN532 FAIL : MAINPN532");
    AllNeoOn(RED);
  }
  else
  {
    nfc[MAINPN532].SAMConfig();
    Serial.println("PN532 SUCC : MAINPN532");
    rfid_init_complete[MAINPN532] = true;
  }
  delay(100);
}

// ptrCurrentMode로 등록되어 loop()마다 호출됨.
// 매 프레임 PN532에 태그 감지를 질의하고, 태그가 감지되면 NTAG의 7번 페이지(사용자 데이터)를
// 읽어 CheckingPlayers()로 넘겨 역할을 판정한다.
// (rfid_num은 현재 1로 고정되어 있어 for문은 사실상 MAINPN532 1개만 순회한다.)
void RfidLoopMain()
{
  uint8_t uid[3][7] = {{0, 0, 0, 0, 0, 0, 0},
                       {0, 0, 0, 0, 0, 0, 0},
                       {0, 0, 0, 0, 0, 0, 0}}; // Buffer to store the returned UID
  uint8_t uidLength[] = {0};                   // Length of the UID (4 or 7 bytes depending on ISO14443A card type)
  uint8_t data[32];
  byte pn532_packetbuffer11[64];
  pn532_packetbuffer11[0] = 0x00;

  for (int i = 0; i < rfid_num; ++i)
  {
    if (nfc[MAINPN532].sendCommandCheckAck(pn532_packetbuffer11, 1)){ // rfid 통신 가능한 상태인지 확인
      if (nfc[MAINPN532].startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A)){                                       // rfid에 tag 찍혔는지 확인용 //데이터 들어오면 uid정보 가져오기
        if (nfc[MAINPN532].ntag2xx_ReadPage(7, data)){ // ntag 데이터에 접근해서 불러와서 data행열에 저장
          Serial.println("TAGGGED");
          CheckingPlayers(data);
        }
      }
    }
  }
}

// 태그에서 읽어온 4바이트("GxPx" 형식의 그룹/플레이어 코드)를 문자열로 조립하고,
// 서버(has2wifi.Receive)에 조회해 그 사용자의 역할(role: player/tagger/ghost)을 받아온 뒤 분기 처리한다.
// - "MMMM" 태그는 스태프용 마스터 카드로, 태그되면 기기를 즉시 재부팅한다(디버깅/리셋용).
// - role이 "player"인 경우에만 ptrRfidMode()를 호출해 실제 게임 진행(예: StartFinish)을 트리거한다.
// - tagger/ghost/그 외 값은 로그만 남기고 별도 동작은 하지 않는다.
void CheckingPlayers(uint8_t rfidData[32]) //어떤 카드가 들어왔는지 확인용
{
  String tagUser = "";
  for(int i = 0; i < 4; i++)    //GxPx 데이터만 배열에서 추출해서 string으로 저장
    tagUser += (char)rfidData[i];
  Serial.println("tag_user_data : " + tagUser);     // 1. 태그한 플레이어의 역할과 생명칩갯수, 최대생명칩갯수 등 읽어오기
  if(tagUser == "MMMM"){  //스태프카드 초기화
    ESP.restart();
  }
  has2wifi.Receive(tagUser);                        // 2. 술래인지, 플레이어인지 구분
  if((String)(const char*)tag["role"] == "player"){ // 3. 태그한 사용자가 플레이어고
    Serial.println("Player Tagged");
    ptrRfidMode();
  }
  else if((String)(const char*)tag["role"] == "tagger"){ // 3. 태그한 사용자가 플레이어고
    Serial.println("Tagger Tagged");
  }
  else if((String)(const char*)tag["role"] == "ghost"){ // 3. 태그한 사용자가 플레이어고
    Serial.println("Ghost Tagged");
  }
  else{
    Serial.println("Wrong TAG");
  }
}


// 배터리팩이 최대치까지 충전 완료됐을 때 호출됨 (WirePollMain 또는 DataChanged의 "battery_max" 분기에서 진입).
// 서버에 "battery_max" 상태를 알리고, 임시 음원을 재생한 뒤, 전체 LED를 초록→파랑으로 전환하고
// 엔코더 값/타이머를 리셋해 다음 단계인 스타터(StarterActivate) 모드로 전환한다.
void BatteryFinish()
{
  has2wifi.Send((String)(const char*)my["device_name"], "device_state", "battery_max"); //메인으로 전송
  Mp3PlayLargeFolder(1, 1);  // TODO: PG_STARTER 음원 지정 ("wStaterOn.en=1" Nextion 위젯 토글은 제거함)
  delay(10);
  AllNeoOn(GREEN);
  Serial.println("Battery Finish Func!");
  encoderValue = 1;
  GameTimer.deleteTimer(gameTimerId);
  gameTimerCnt = 0;
  gameTimerId = GameTimer.setInterval(gameTime,GameTimerFunc); // 방치 시 게이지 감소 타이머 재시작
  AllNeoOn(BLUE);
  EncoderAttach();  // 엔코더 카운팅 시작 — 이제부터 손잡이를 돌리면 encoderValue가 증가함
  delay(100);
  ptrCurrentMode = StarterActivate; // 다음 loop부터 스타터 미니게임 루프로 전환
}

// 스타터(엔코더) 게이지가 가득 차서 발전기 수리가 완료됐을 때 호출됨 (Game_system.ino의 StarterActivate에서 진입).
// 진행 중이던 타이머들을 정리하고 서버에 "repaired" 상태를 알린 뒤,
// 전체 발전기가 다 고쳐졌다면("repaired_all") 탈출구 오픈 처리를, 아니라면 남은 발전기 개수 안내 후
// 다시 대기(WaitFunc) 상태로 돌아간다.
void StartFinish()
{
  Serial.println("StartFinish PTRFUNC");
  GameTimer.deleteTimer(gameTimerId);        //게임 타이머 종료3
  BlinkTimer.deleteTimer(blinkTimerId);
  Serial.println("Generator Fixed!");
  has2wifi.Send((String)(const char*)my["device_name"], "device_state", "repaired");
  receiveMineOn = true;       // 서버 응답을 다시 폴링해서 아래 있는 device_state를 최신값으로 받아옴
  has2wifi.ReceiveMine();
  if ((String)(const char*)my["device_state"] == "repaired_all") {
    ptrRfidMode = WaitFunc;
    ptrCurrentMode = WaitFunc;
    Mp3PlayLargeFolder(1, 1);  // TODO: PG_ESCAPE_OPEN 음원 지정
    BlinkTimer.deleteTimer(blinkTimerId);
    AllNeoOn(BLUE);
    return;
  }
  Mp3PlayLargeFolder(1, 1);  // TODO: PG_FIXED 음원 지정
  LeftGenerator();
  AllNeoOn(BLUE);
  ptrCurrentMode = WaitFunc;
}
