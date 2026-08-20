// error_recovery.ino
// 런타임 에러 감지 및 복구 로직.
// 설계 원칙: Beetle silence는 오류가 아님. bad event(unknown cmd / 포맷 오류 /
// 파싱 실패) 가 연속 3사이클 누적될 때만 UART 재초기화.

// ---------------------------------------------------------
// ResetBeetleErrorCounters: 유효 패킷 수신 또는 복구 후 오류 카운터 초기화.
// serial_Communication.ino의 정상 T 패킷 처리 성공 시 호출.
// RecoverBeetleConnection() 내부에서도 호출.
// ---------------------------------------------------------
void ResetBeetleErrorCounters() {
  invalidCmdCount = 0;
  packetFormatErrorCount = 0;
  tagParseErrorCount = 0;
  beetleBadEventStreak = 0;
}

// ---------------------------------------------------------
// RecoverBeetleConnection: Beetle UART 재초기화.
// silence가 아닌 bad event 3회 누적 시에만 호출.
// 재초기화 후 RecoverToLastStableState() 호출.
// ---------------------------------------------------------
void RecoverBeetleConnection() {
  beetleRecoverAttempts++;
  Serial.println("[RECOVERY] Beetle UART 재초기화 시도 #" +
                 String(beetleRecoverAttempts));
  toSubSerial.end();
  delay(100);
  toSubSerial.begin(115200, SERIAL_8N1, HWSERIAL_RX, HWSERIAL_TX);
  for (int i = 0; i < 3; i++) {
    toSubSerial.println("R");
    delay(50);
  }
  toSubSerial.println("W");
  ResetBeetleErrorCounters();
  Serial.println("[RECOVERY] UART 재초기화 완료.");
}

// ---------------------------------------------------------
// HandleRuntimeRecovery: 매 GameTimerFunc / WifiIntervalFunc 호출 시 실행.
// bad event 누적 → 3사이클 연속 시 RecoverBeetleConnection
// ※ Beetle silence(무수신)는 bad event로 간주하지 않음.
// ---------------------------------------------------------
void HandleRuntimeRecovery() {
  // --- bad event 누적 감지 (delta 방식, silence 제외) ---
  static int prevCmd = 0, prevFmt = 0, prevTag = 0;

  // 외부에서 카운터가 감소한 경우(ResetBeetleErrorCounters 호출) prev 동기화
  if (invalidCmdCount < prevCmd)
    prevCmd = invalidCmdCount;
  if (packetFormatErrorCount < prevFmt)
    prevFmt = packetFormatErrorCount;
  if (tagParseErrorCount < prevTag)
    prevTag = tagParseErrorCount;

  bool newBadEvent =
      (invalidCmdCount > prevCmd || packetFormatErrorCount > prevFmt ||
       tagParseErrorCount > prevTag);

  prevCmd = invalidCmdCount;
  prevFmt = packetFormatErrorCount;
  prevTag = tagParseErrorCount;

  if (newBadEvent) {
    beetleBadEventStreak++;
    Serial.println(
        "[RECOVERY] bad event streak=" + String(beetleBadEventStreak) +
        " (cmd=" + String(invalidCmdCount) +
        " fmt=" + String(packetFormatErrorCount) +
        " tag=" + String(tagParseErrorCount) + ")");
  }

  if (beetleBadEventStreak >= 3) {
    RecoverBeetleConnection();
    prevCmd = 0;
    prevFmt = 0;
    prevTag = 0;
    if (beetleRecoverAttempts >= 3) {
      Serial.println("[RECOVERY] 최대 복구 시도 초과. 재시작합니다.");
      delay(500);
      ESP.restart();
    }
  }
}

// ---------------------------------------------------------
// SendDeviceStateWithRetry: device_state 전송 재시도 래퍼.
// WiFi 연결 상태 확인 후 retries 회 시도. 실패 시 로그만 남김.
// ---------------------------------------------------------
bool SendDeviceStateWithRetry(const String &value, uint8_t retries) {
  for (uint8_t i = 0; i < retries; i++) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] WARN: SendDeviceState '" + value + "' attempt " +
                     String(i + 1) + " skipped (no WiFi)");
      delay(200);
      continue;
    }
    has2wifi.Send((String)(const char *)my["device_name"], "device_state",
                  value);
    Serial.println("[WIFI] SendDeviceState '" + value + "' sent (attempt " +
                   String(i + 1) + ")");
    return true;
  }
  Serial.println("[WIFI] WARN: SendDeviceState '" + value +
                 "' all retries failed.");
  return false;
}
