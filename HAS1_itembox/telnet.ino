//****************************************TELNET (무선 디버그 로그)****************************************************************
// WiFi 연결 시 텔넷(포트 23)으로 로그 미러링. 실제 출력은 Log()가 담당하고, 이 파일은 연결 관리만 한다.
// WiFi가 없어도 게임은 정상 동작 — 연결 시도는 논블로킹이고, 로그는 시리얼로 항상 나간다.
// 접속 방법: 같은 WiFi에 붙은 PC에서  telnet <보드IP> 23

// WiFi 연결 시도 시작 (블로킹 대기 없음 — 백그라운드에서 알아서 붙는다)
void TelnetInit() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);          // 공유기가 잠깐 죽어도 자동 재접속
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  telnetServer.begin();
  telnetServer.setNoDelay(true);
  Log("NET", "wifi connecting: " + String(WIFI_SSID));
}

// loop()에서 매번 호출. WiFi 상태 변화 감지 + 텔넷 클라이언트 접속 처리
void TelnetRun() {
  static bool wifiWasConnected = false;
  bool wifiConnected = (WiFi.status() == WL_CONNECTED);

  if (wifiConnected && !wifiWasConnected)
    Log("NET", "wifi connected, telnet: " + WiFi.localIP().toString() + ":23");
  else if (!wifiConnected && wifiWasConnected)
    Log("NET", "wifi disconnected (serial only)");
  wifiWasConnected = wifiConnected;

  if (!wifiConnected) return;

  // 새 클라이언트 접속 처리 (동시 1명만 허용)
  if (telnetServer.hasClient()) {
    WiFiClient newClient = telnetServer.available();
    if (telnetClient && telnetClient.connected()) {
      newClient.println("telnet already in use");
      newClient.stop();
    } else {
      telnetClient = newClient;
      telnetClient.setNoDelay(true);
      Log("NET", "telnet client connected");
    }
  }

  // 끊긴 클라이언트 정리
  if (telnetClient && !telnetClient.connected())
    telnetClient.stop();

  // 클라이언트가 보낸 입력은 버린다 (로그 출력 전용)
  while (telnetClient && telnetClient.connected() && telnetClient.available())
    telnetClient.read();
}
