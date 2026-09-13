// 모터 동작처럼 수 초간 블로킹한 직후에만 쓰는 RX 버퍼 비우기.
// 그 사이 쌓인 줄은 이미 수 초 묵은 값이라 처리해도 의미가 없고, MMMM의 경우
// 오히려 중복 토글을 일으킨다. 주의: toSubSerial.flush()는 TX만 비우므로
// (ESP32 코어의 uartFlushTxOnly) RX를 치우려면 이렇게 읽어내야 한다.
// 상시 드레인은 유효한 패킷을 버리므로 절대 하지 않는다.
void DrainSubSerial(){
  while(toSubSerial.available()) toSubSerial.read();
}

// Beetle은 loop()에 딜레이가 없어서 스캔 속도대로 T 패킷을 계속 밀어넣는다(실측: 리더 3개
// 스캔마다 1줄). TTGO는 500ms마다 한 번 읽으므로 매 주기 여러 줄이 쌓인다. 예전에는 쌓인 줄을
// 앞에서부터 전부 처리했는데, T는 "지금 각 리더에 뭐가 올려져 있나"의 스냅샷이라 묵은 것을
// 처리해봐야 의미가 없고 비용만 크다 — PlayerDetector가 태그 1개당 has2wifi.Receive()
// (블로킹 HTTP 왕복 221~336ms, 타임아웃 미설정 시 최악 5초)를 부르기 때문에, 리더에 카드가
// 올려져 있으면 한 주기에 HTTP가 수십 번 나가 loop가 수 초간 묶였다.
//
// 그래서 읽기와 처리를 분리한다. 1단계에서 버퍼를 끝까지 비워 분류만 하고, 2단계에서
// 최신 T 한 줄만 처리한다. 주의: 'M'은 상태를 토글하는 1회성 이벤트라 버리면 태그가
// 아무 일도 안 한 것이 되므로 절대 최신값 덮어쓰기 대상이 아니다. 별도 플래그로 살린다.
void CommnunicationBeetle(){
  Serial.println("READ");

  // --- 1단계: 쌓인 줄을 전부 읽어 분류만 한다 (여기서는 HTTP/모터 같은 무거운 일 없음) ---
  const uint8_t kMaxLinesPerCall = 16;
  String latestT = "";        // T 스냅샷: 뒤엣것이 앞엣것을 덮어쓴다
  bool   mmmmSeen = false;    // M 이벤트: 한 번이라도 봤으면 살린다

  for (uint8_t processed = 0; processed < kMaxLinesPerCall; ++processed){
    if(toSubSerial.available() <= 0) break;   // 버퍼가 비면 종료
    lastBeetleMs = millis();
    String command = toSubSerial.readStringUntil('\n');

    if (command.length() == 0) continue;      // 빈 문자열 방어
    char cmd = command[0];

    if(cmd == 'T'){
      latestT = command;
    }
    else if(cmd == 'M'){
      mmmmSeen = true;
    }
    else if(cmd == 'W'){
      Serial.println("Beetle Init Success");
      toSubSerial.println("W");
    }
    else if(cmd == 'R'){
      Serial.println("Beetle Reset Success");
    }
    else if(cmd == 'B'){
      Serial.println(command);
    }
    else {
      // 허용되지 않은 명령 문자
      invalidCmdCount++;
      Serial.println("[UART] WARN unknown command '" + String(cmd) + "'");
    }
  }

  // --- 2단계: M을 먼저 처리한다. 상태가 바뀌면 같은 패스에서 모은 T는 전환 이전의
  // 스냅샷이라 이미 묵은 값이므로 그대로 버린다. ---
  if (mmmmSeen){
    HandleMmmmCard();
    return;
  }
  if (latestT.length() > 0){
    HandleTagPacket(latestT);
  }
}

// MMMM 관리자 카드. 이전에는 로컬 static bool 토글로 activate/ready를 번갈아 호출했는데,
// 서버도 DataChanged()(wifi.ino)에서 같은 ActivateFunc/ReadyFunc를 독립적으로 호출하기
// 때문에 서버가 상태를 바꾸면 토글 위상이 어긋나 다음 카드 한 번이 반대로 동작했다
// (현장: "MMMM 카드 제대로 작동 안함"). 사설 상태 대신 현재 device_state에서 도출한다.
void HandleMmmmCard(){
  // 카드가 얹혀 있는 동안 'M'이 계속 오므로, 마지막으로 본 시각을 항상 갱신한다.
  // 폴링 주기(500ms)가 재무장 시간(1500ms)보다 짧으므로, 카드를 떼지 않는 한 창은 계속 닫혀 있다.
  unsigned long nowMs = millis();
  bool rearmed = (lastMmmmSeenMs == 0) || (nowMs - lastMmmmSeenMs > MMMM_REARM_MS);
  lastMmmmSeenMs = nowMs;
  if(!rearmed){
    return;   // 아직 같은 태그로 본다 (카드를 떼야 재무장)
  }

  if((String)(const char*)my["device_state"] == "activate"){
    ReadyFunc();
    SendDeviceStateWithRetry("ready");
    SendStateWithRetry("game_state", "ready");
    // MMMM은 device_state뿐 아니라 game_state도 함께 옮긴다. 둘이 갈라지면 DataChanged의
    // game_state 분기(ActivateFunc/ReadyFunc)가 나중에 따로 한 번 더 튄다.
    // 이 전환은 방금 로컬에서 적용했으므로 cur에도 맞춰둔다. 안 맞추면 다음 서버 폴링에서
    // DataChanged가 처음 보는 변경으로 오인해 ActivateFunc를 한 번 더 부른다 (실측: 문이
    // 4초 열리고 곧바로 다시 4초 열려 총 8초). device_state 분기에는 "ready"가 없어서
    // 닫기 방향에는 이 중복이 없다 — 현장의 "ready->activate만 느림"이 이것이다.
    // Send()는 서버로만 보내고 로컬 my를 갱신하지 않는다. 그대로 두면 다음 폴링 전까지
    // 여전히 "activate"로 읽혀 연타 시 같은 분기를 반복한다.
    my["device_state"] = "ready";
    cur["device_state"] = "ready";
    my["game_state"] = "ready";
    cur["game_state"] = "ready";
  } else {
    ActivateFunc();
    SendDeviceStateWithRetry("activate");
    SendStateWithRetry("game_state", "activate");
    my["device_state"] = "activate";
    cur["device_state"] = "activate";
    my["game_state"] = "activate";
    cur["game_state"] = "activate";
  }

  // 모터가 도는 4~6초 동안 쌓인 줄은 전부 묵은 값이다. 버리지 않으면
  // 그 안의 'M'들이 곧바로 재처리되어 상태가 다시 뒤집힌다.
  DrainSubSerial();
  lastMmmmSeenMs = millis();   // 드레인 직후부터 재무장 시간을 다시 잰다
}

void HandleTagPacket(String command){
  // 원문 저장 (포맷 검증 전)
  lastBeetleRawPacket = command;

  // --- 포맷 검증: "T1:xxxx_T2:xxxx_T3:xxxx" (최소 길이 23, 구분자 위치 고정) ---
  bool fmtOk = (command.length() >= 23 &&
                command[1] == '1' && command[2] == ':' &&
                command[7] == '_' &&
                command[8] == 'T' && command[9] == '2' && command[10] == ':' &&
                command[15] == '_' &&
                command[16] == 'T' && command[17] == '3' && command[18] == ':');

  if (!fmtOk) {
    packetFormatErrorCount++;
    Serial.println("[UART] WARN malformed T packet: " + command);
    return;
  }

  Serial.println(command);
  tag1 = command.substring(3, 7);
  tag2 = command.substring(11, 15);
  tag3 = command.substring(19, 23);

  Serial.println("TAG1 = " + tag1);
  Serial.println("TAG2 = " + tag2);
  Serial.println("TAG3 = " + tag3);

  tagState[0] = PlayerDetector(tag1);
  tagState[1] = PlayerDetector(tag2);
  tagState[2] = PlayerDetector(tag3);

  // 유효 패킷 처리 성공 → bad event 카운터 초기화
  ResetBeetleErrorCounters();

  // 태그 코드만 '_' 로 join → URL-safe(영숫자+언더바)라 인코딩 불필요.
  // 예: "G1P1_GxP0_GxP0". 서버는 explode("_", value) 로 배열 파싱.
  // 3개 다 "GxP0"(미검출)면 매 루프 동일값 재전송이라 스팸이므로 스킵하고,
  // 하나라도 실제 태그(마지막 자리 != '0')면 전송한다.
  // 여기서 바로 Send(HTTP 왕복)하면 오디오 재생(TagCount)이 그만큼 늦어지므로
  // 값만 보관해두고 실제 전송은 오디오가 나간 뒤 FlushPendingTagSend()에서 한다.
  bool hasMeaningfulTag = (tag1[3] != '0') || (tag2[3] != '0') || (tag3[3] != '0');
  if (hasMeaningfulTag) {
    pendingTagValue = tag1 + "_" + tag2 + "_" + tag3;
    tagValuePending = true;
  }
}

void FlushPendingTagSend(){
  if (!tagValuePending) return;
  tagValuePending = false;
  has2wifi.Send((String)(const char*)my["device_name"], "tagged_players", pendingTagValue);
}

bool PlayerDetector(String playerNum)
{
  // 길이 방어: playerNum[3] 접근 전 확인
  if (playerNum.length() < 4) {
    tagParseErrorCount++;
    Serial.println("[UART] WARN tag length < 4: '" + playerNum + "'");
    return false;
  }

  if(playerNum[3] == '0')
    return false;

  has2wifi.Receive(playerNum);
  String role = (String)(const char*)tag["role"];

  // fake/tagger(디코이) 상태에서는 누가(player/ghost/revival) 찍히든 전용 오디오만 재생한다.
  // 어떤 오디오인지는 태그한 사람의 role이 아니라 이 장치 자신의 device_state로 결정:
  // tagger 상태면 (1,7), fake 상태면 (1,8).
  String deviceState = (String)(const char*)my["device_state"];
  if (deviceState == "fake" || deviceState == "tagger") {
    if (role == "player" || role == "ghost" || role == "revival") {
      Mp3PlayLargeFolder(1, deviceState == "tagger" ? 7 : 8);
      AllNeoBlink(PURPLE, 3, 150);
      return false;
    }
  }

  if (role == "player") {
    return true;
  } else if (role == "tagger") {
    return false;
  } else {
    // role 미해석: 서버 미등록 태그 또는 UART 노이즈
    tagParseErrorCount++;
    Serial.println("[UART] WARN tag role unresolved: '" + role + "' for " + playerNum);
    return false;
  }
}
