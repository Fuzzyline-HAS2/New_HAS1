#include <lwip/sockets.h>

WiFiServer telnetServer(23);
WiFiClient telnetClient;
HardwareSerial HardwareDebugSerial(0);
TelnetDebugConsole DebugSerial;

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

// Emit a bounded snapshot only when a remote diagnostic client connects.
// This runs on the Arduino loop task, the same owner as the BLE state machine.
static void TelnetReportDiagnostics() {
  const int socketFd = telnetClient.fd();
  if (socketFd < 0) return;
  const Has1BleBeacon::Diagnostics ble = Has1BleBeacon::diagnostics();
  const char *deviceName = my["device_name"].as<const char *>();
  uint8_t mac[6] = {};
  WiFi.macAddress(mac);
  const IPAddress ip = WiFi.localIP();
  char buffer[384];
  const int length = snprintf(
      buffer, sizeof(buffer),
      "[diagnostics] firmware=%u device=%.18s "
      "mac=%02X:%02X:%02X:%02X:%02X:%02X ip=%u.%u.%u.%u "
      "heap_free=%lu heap_min=%lu heap_largest=%lu "
      "ble_step=%u ble_radio=%u ble_error=%u ble_hci=%u "
      "ble_pending=%u ble_awaiting=%u ble_timeout=%u "
      "ble_commands=%lu ble_failures=%lu\r\n",
      static_cast<unsigned>(FIRMWARE_VER), deviceName ? deviceName : "",
      static_cast<unsigned>(mac[0]), static_cast<unsigned>(mac[1]),
      static_cast<unsigned>(mac[2]), static_cast<unsigned>(mac[3]),
      static_cast<unsigned>(mac[4]), static_cast<unsigned>(mac[5]),
      static_cast<unsigned>(ip[0]), static_cast<unsigned>(ip[1]),
      static_cast<unsigned>(ip[2]), static_cast<unsigned>(ip[3]),
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(ESP.getMinFreeHeap()),
      static_cast<unsigned long>(ESP.getMaxAllocHeap()),
      static_cast<unsigned>(ble.step), static_cast<unsigned>(ble.radio),
      static_cast<unsigned>(ble.lastError), static_cast<unsigned>(ble.hciStatus),
      static_cast<unsigned>(ble.pendingOpcode),
      static_cast<unsigned>(ble.awaitingCompletion),
      static_cast<unsigned>(ble.completionTimedOut),
      static_cast<unsigned long>(ble.sentCommands),
      static_cast<unsigned long>(ble.failures));
  if (length > 0 && static_cast<size_t>(length) < sizeof(buffer)) {
    // One best-effort attempt: a slow reader must never hold up device timers.
    // Drop failed/partial snapshots; reconnect to request another snapshot.
    (void)send(socketFd, buffer, static_cast<size_t>(length), MSG_DONTWAIT);
  }
}

void TelnetInit() {
  telnetServer.begin();
  telnetServer.setNoDelay(true);
  DebugSerial.print("Telnet ready: ");
  DebugSerial.print(WiFi.localIP());
  DebugSerial.println(":23");
}

void TelnetRun() {
  if (telnetServer.hasClient()) {
    WiFiClient newClient = telnetServer.available();

    if (telnetClient && telnetClient.connected()) {
      newClient.println("Telnet already connected.");
      newClient.stop();
      return;
    }

    telnetClient = newClient;
    telnetClient.setNoDelay(true);
    DebugSerial.println("Telnet client connected");
    TelnetReportDiagnostics();
  }

  if (telnetClient && !telnetClient.connected()) {
    telnetClient.stop();
    DebugSerial.println("Telnet client disconnected");
  }

  while (telnetClient && telnetClient.connected() && telnetClient.available()) {
    char c = telnetClient.read();
    HardwareDebugSerial.write(c);
  }
}
