void CommnunicationBeetle(){
  Serial.println("READ");
  if(toSubSerial.available() > 0){
    lastBeetleMs = millis();
    String command = toSubSerial.readStringUntil('\n');

    // 빈 문자열 방어
    if (command.length() == 0) return;

    char cmd = command[0];

    if(cmd == 'W'){
      Serial.println("Beetle Init Success");
      toSubSerial.println("W");
    }
    else if(cmd == 'R'){
      Serial.println("Beetle Reset Success");
    }
    else if(cmd == 'T'){
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
        return; // 파싱 금지 — 잘못된 substring 접근 방지
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
    else if(cmd == 'B'){
      Serial.println(command);
    }
    else if(cmd == 'M'){
      static bool escapeIsOpen = false;
      if(escapeIsOpen){
        EscapeClose();
        escapeIsOpen = false;
      } else {
        EscapeOpen();
        escapeIsOpen = true;
      }
    }
    else {
      // 허용되지 않은 명령 문자
      invalidCmdCount++;
      Serial.println("[UART] WARN unknown command '" + String(cmd) + "'");
    }
  }
  while(toSubSerial.available()){
    toSubSerial.read();
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
