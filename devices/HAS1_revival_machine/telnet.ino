WiFiServer telnetServer(23);
WiFiClient telnetClient;
HardwareSerial HardwareDebugSerial(0);
TelnetDebugConsole DebugSerial;
static bool cardUploadTelnetConnected = false;

void CardUploadOutput(const char *line) {
  if (telnetClient && telnetClient.connected()) telnetClient.println(line);
}

void TelnetDebugConsole::begin(unsigned long baud) {
  HardwareDebugSerial.begin(baud);
}

int TelnetDebugConsole::available() {
  return HardwareDebugSerial.available();
}

int TelnetDebugConsole::read() {
  return HardwareDebugSerial.read();
}

int TelnetDebugConsole::peek() {
  return HardwareDebugSerial.peek();
}

void TelnetDebugConsole::flush() {
  HardwareDebugSerial.flush();
}

size_t TelnetDebugConsole::write(uint8_t data) {
  HardwareDebugSerial.write(data);
  if (telnetClient && telnetClient.connected()) {
    telnetClient.write(data);
  }
  return 1;
}

size_t TelnetDebugConsole::write(const uint8_t *buffer, size_t size) {
  HardwareDebugSerial.write(buffer, size);
  if (telnetClient && telnetClient.connected()) {
    telnetClient.write(buffer, size);
  }
  return size;
}

void TelnetInit() {
  telnetServer.begin();
  telnetServer.setNoDelay(true);
  DebugSerial.print("Telnet ready: ");
  DebugSerial.print(WiFi.localIP());
  DebugSerial.println(":23");
}

void TelnetRun() {
  if (cardUploadTelnetConnected && !telnetClient.connected()) {
    CardUploadDisconnected();
    cardUploadTelnetConnected = false;
    telnetClient.stop();
  }
  if (telnetServer.hasClient()) {
    WiFiClient newClient = telnetServer.available();

    if (telnetClient && telnetClient.connected()) {
      newClient.println("Telnet already connected.");
      newClient.stop();
      return;
    }

    telnetClient = newClient;
    telnetClient.setNoDelay(true);
    cardUploadTelnetConnected = true;
    CardUploadConnected();
    DebugSerial.println("Telnet client connected");
  }

  if (telnetClient && !telnetClient.connected()) {
    telnetClient.stop();
    DebugSerial.println("Telnet client disconnected");
  }

  // A pasted line or Telnet negotiation must not starve server/state handling.
  for (uint8_t count = 0; count < 64 && telnetClient && telnetClient.connected() && telnetClient.available(); ++count) {
    CardUploadInput((uint8_t)telnetClient.read());
  }
}
