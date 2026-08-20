// 태그 ID는 항상 숫자를 포함(G1P0 등, 예외는 스태프카드 MMMM)하는데 반해
// D: 진단 메시지가 시리얼 손상으로 앞글자를 잃으면 순수 알파벳 문자열만 남는다.
// 그런 경우 태그로 오인해 CheckingPlayers를 부르지 않도록 걸러낸다.
bool LooksLikeTag(const String &command) {
  if (command == "MMMM") return true;
  for (unsigned int i = 0; i < command.length(); i++) {
    if (isDigit(command[i])) return true;
  }
  return false;
}

void CommnunicationBeetle(){
  // Serial.println("READ");
  // 태그 1회 인식마다 비틀이 두 줄(태그데이터/D:상태)을 연달아 보낼 수 있으므로
  // 버퍼에 쌓인 줄을 전부 처리한다 (한 줄만 처리하고 나머지를 flush하면 유실됨)
  while(toSubSerial.available() > 0){
    String command = toSubSerial.readStringUntil('\n'); //추가 시리얼의 값을 수신하여 String으로 저장
    if(command.length() == 0) continue;
    Serial.println(command);
    // Serial.println("received:" + String(command[0])); //기본 시리얼에 추가 시리얼 내용을 출력

    if(command[0] == 'W'){
      Serial.println("Beetle Init Success");
      toSubSerial.println("W");
    }
    else if(command[0] == 'R'){
      Serial.println("Beetle Reset Success");
    }
    else if(command[0] == 'M'){
      digitalWrite(RELAY_PIN, HIGH);
      delay(500);
      digitalWrite(RELAY_PIN, LOW);
    }
    else if(command[0] == 'D'){   // PN532 진단 로그 (예: D:COLL, D:PARITY) - 태그 데이터로 처리하지 않고 텔넷 출력만
      Serial.println("[Sub Beetle PN532] " + command.substring(2));
    }
    else if(command.length() >= 2 && LooksLikeTag(command)){   // NFC 태그 데이터 (시리얼 손상으로 최대 2글자 유실돼도 태그로 처리, 예: G1P1->P0)
      mainRfidTagged = false;
      Serial.println("Sub Beetle Tag: " + command.substring(0,4));
      CheckingPlayers(command.substring(0,4));        // 역할 조회 후 네오픽셀 연출 시작
      SendBeetleTag(BEETLE_SUB, "sub_beetle_player", command.substring(0,4));  // 서버에는 비동기로 즉시 전송(연출 안 막음)
      if(SubSerialTimerStart == true){
        SubSerialTimer.deleteTimer(subSerialTimerId);
        SubSerialTimerStart = false;
      }
      subSerialTimerId = SubSerialTimer.setInterval(1000,SubSerialTimerFunc);
      SubSerialTimerStart = true;
    }
  }
}

/**
 * @brief 비틀 태그값 서버 즉시 전송 (채널별 1초 스로틀, 스태프 카드 제외)
 * 서버는 위치를 바로 받아야 하므로 지연 없이 보내되, 메인 루프(게이지 애니메이션)를
 * 막지 않도록 SendAsync(백그라운드 태스크)로 처리
 * @param idx BEETLE_SUB / BEETLE_MAIN
 */
void SendBeetleTag(int idx, const char *field, const String &tagUser){
  if(tagUser == "MMMM") return;   // 스태프 카드는 서버 전송 제외
  unsigned long now = millis();
  if(beetleSendLastMs[idx] != 0 && now - beetleSendLastMs[idx] < 1000) return;
  beetleSendLastMs[idx] = now;
  has2wifi.SendAsync((String)(const char *)my["device_name"], field, tagUser);
}

void SubSerialFlush(){
  while(toSubSerial.available())      //시리얼 통신 버퍼 flush
      toSubSerial.read();
}

void MainSerialFlush(){
  while(toMainSerial.available())     //Main Beetle 시리얼 통신 버퍼 flush
      toMainSerial.read();
}

/**
 * @brief Main Beetle 시리얼 통신 처리 (Sub Beetle의 CommnunicationBeetle과 동일 구조)
 * WifiIntervalFunc에서 호출되어 모든 게임 상태에서 W 핸드셰이크 처리
 * activate 상태에서는 WhichTagged → ptrRfidMain에서도 호출되어 태그 데이터 처리
 */
void CommnunicationMainBeetle(){
  // 태그 1회 인식마다 비틀이 두 줄(태그데이터/D:상태)을 연달아 보낼 수 있으므로
  // 버퍼에 쌓인 줄을 전부 처리한다 (한 줄만 처리하고 나머지를 flush하면 유실됨)
  while(toMainSerial.available() > 0){
    String command = toMainSerial.readStringUntil('\n');
    if(command.length() == 0) continue;
    Serial.println("Main Beetle: " + command);

    if(command[0] == 'W'){
      Serial.println("Main Beetle Init Success");
      toMainSerial.println("W");
    }
    else if(command[0] == 'R'){
      Serial.println("Main Beetle Reset Success");
    }
    else if(command[0] == 'M'){
      digitalWrite(RELAY_PIN, HIGH);
      delay(500);
      digitalWrite(RELAY_PIN, LOW);
    }
    else if(command[0] == 'D'){   // PN532 진단 로그 (예: D:COLL, D:PARITY) - 태그 데이터로 처리하지 않고 텔넷 출력만
      Serial.println("[Main Beetle PN532] " + command.substring(2));
    }
    else if(command.length() >= 2 && LooksLikeTag(command)){   // NFC 태그 데이터 (시리얼 손상으로 최대 2글자 유실돼도 태그로 처리, 예: G1P1->P0)
      mainRfidTagged = true;
      Serial.println("TAGGGED (Main Beetle)");
      CheckingPlayers(command.substring(0, 4));       // 역할 조회 후 네오픽셀 연출 시작
      SendBeetleTag(BEETLE_MAIN, "main_beetle_player", command.substring(0, 4));  // 서버에는 비동기로 즉시 전송(연출 안 막음)
      if(SubSerialTimerStart == true){
        SubSerialTimer.deleteTimer(subSerialTimerId);
        SubSerialTimerStart = false;
      }
      subSerialTimerId = SubSerialTimer.setInterval(1000, SubSerialTimerFunc);
      SubSerialTimerStart = true;
    }
  }
}