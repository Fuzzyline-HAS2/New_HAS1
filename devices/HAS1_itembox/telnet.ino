#include <WiFi.h>

WiFiServer telnetServer(23);
WiFiClient telnetClient;

void Log(const char *tag, const String &msg) {
    char head[12];
    snprintf(head, sizeof(head), "[%-5s] ", tag);
    Serial.print(head);
    Serial.println(msg);
    if (telnetClient && telnetClient.connected()) {
        telnetClient.print(head);
        telnetClient.println(msg);
    }
}

// WiFi 연결은 WifiInit()의 has2wifi.Setup()이 담당 — 여기서는 서버만 시작
void TelnetInit() {
    telnetServer.begin();
    telnetServer.setNoDelay(true);
    Log("NET", "telnet server ready");
}

void TelnetRun() {
    static bool wifiWasConnected = false;
    bool wifiConnected = (WiFi.status() == WL_CONNECTED);

    if (wifiConnected && !wifiWasConnected)
        Log("NET", "wifi connected, telnet: " + WiFi.localIP().toString() + ":23");
    else if (!wifiConnected && wifiWasConnected)
        Log("NET", "wifi disconnected (serial only)");
    wifiWasConnected = wifiConnected;

    if (!wifiConnected) return;

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

    if (telnetClient && !telnetClient.connected())
        telnetClient.stop();

    while (telnetClient && telnetClient.connected() && telnetClient.available())
        telnetClient.read();
}
