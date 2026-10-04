#include <lwip/sockets.h>
#include <errno.h>
#include <esp_log.h>
#include <stdarg.h>
#include "telnet_log_buffer.h"

WiFiServer telnetServer(23);
WiFiClient telnetClient;
HardwareSerial HardwareDebugSerial(0);
TelnetDebugConsole DebugSerial;

// Producers include HAS2_Wifi's asynchronous worker and ESP-IDF logging tasks.
// Only fixed-size memory copies happen under this lock; never UART/TCP/printf.
static portMUX_TYPE telnetLogMux = portMUX_INITIALIZER_UNLOCKED;
static TelnetLogBuffer<8192> telnetHistory;
static vprintf_like_t telnetPreviousVprintf = nullptr;
static char telnetRetainedCrash[200] = {};

// Session state belongs exclusively to the Arduino loop task.
static bool telnetSessionActive = false;
static uint64_t telnetPosition = 0;
static uint64_t telnetReplayEnd = 0;
static bool telnetReplayEnded = false;
static char telnetControl[1024];
static size_t telnetControlLength = 0;
static size_t telnetControlSent = 0;

static void TelnetRemember(const uint8_t *buffer, size_t size) {
  while (size) {
    const size_t chunk = size < 128 ? size : 128;
    portENTER_CRITICAL(&telnetLogMux);
    telnetHistory.append(buffer, chunk);
    portEXIT_CRITICAL(&telnetLogMux);
    buffer += chunk;
    size -= chunk;
  }
}

static int TelnetVprintf(const char *format, va_list arguments) {
  char buffer[512];
  va_list copy;
  va_copy(copy, arguments);
  const int length = vsnprintf(buffer, sizeof(buffer), format, copy);
  va_end(copy);
  if (length > 0) {
    const size_t kept = static_cast<size_t>(length) < sizeof(buffer)
                            ? static_cast<size_t>(length) : sizeof(buffer) - 1;
    TelnetRemember(reinterpret_cast<const uint8_t *>(buffer), kept);
    if (static_cast<size_t>(length) >= sizeof(buffer)) {
      static const char truncated[] = "\r\n[Telnet] ESP-IDF log truncated at 511 bytes\r\n";
      TelnetRemember(reinterpret_cast<const uint8_t *>(truncated), sizeof(truncated) - 1);
    }
  }
  // Preserve the pre-existing UART/log sink. Do not call DebugSerial here:
  // that would mirror the same message twice and could recurse into logging.
  return telnetPreviousVprintf ? telnetPreviousVprintf(format, arguments) : length;
}

void TelnetDebugConsole::begin(unsigned long baud) {
  HardwareDebugSerial.begin(baud);
  static bool hookInstalled = false;
  if (!hookInstalled) {
    telnetPreviousVprintf = esp_log_set_vprintf(TelnetVprintf);
    hookInstalled = true;
  }
}

int TelnetDebugConsole::available() { return HardwareDebugSerial.available(); }
int TelnetDebugConsole::read() { return HardwareDebugSerial.read(); }
int TelnetDebugConsole::peek() { return HardwareDebugSerial.peek(); }
void TelnetDebugConsole::flush() { HardwareDebugSerial.flush(); }

size_t TelnetDebugConsole::write(uint8_t data) { return write(&data, 1); }
size_t TelnetDebugConsole::write(const uint8_t *buffer, size_t size) {
  if (!buffer || !size) return 0;
  // Remember before UART output, so a delayed UART does not hide the last log.
  TelnetRemember(buffer, size);
  HardwareDebugSerial.write(buffer, size);
  return size;
}

void TelnetPreserveCrashReport() {
  // CrashReportSend later sanitizes g_crash_msg in place for a URL. Keep the
  // original independently of the rolling history and replay on every connect.
  if (g_crash_telnet_pending) {
    snprintf(telnetRetainedCrash, sizeof(telnetRetainedCrash), "%s", g_crash_msg);
    g_crash_telnet_pending = false;
  }
}

static size_t TelnetFormatDiagnostics(char *buffer, size_t capacity) {
  const Has1BleBeacon::Diagnostics ble = Has1BleBeacon::diagnostics();
  const char *deviceName = my["device_name"].as<const char *>();
  uint8_t mac[6] = {};
  WiFi.macAddress(mac);
  const IPAddress ip = WiFi.localIP();
  const int length = snprintf(
      buffer, capacity,
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
  return length > 0 && static_cast<size_t>(length) < capacity ? static_cast<size_t>(length) : 0;
}

void TelnetInit() {
  telnetServer.begin();
  telnetServer.setNoDelay(true);
  DebugSerial.print("Telnet ready: ");
  DebugSerial.print(WiFi.localIP());
  DebugSerial.println(":23");
}

static void TelnetStartSession() {
  portENTER_CRITICAL(&telnetLogMux);
  telnetPosition = telnetHistory.oldest();
  telnetReplayEnd = telnetHistory.end();
  portEXIT_CRITICAL(&telnetLogMux);
  telnetReplayEnded = false;
  telnetControlSent = 0;
  telnetControlLength = TelnetFormatDiagnostics(telnetControl, sizeof(telnetControl));
  const int extra = snprintf(telnetControl + telnetControlLength,
      sizeof(telnetControl) - telnetControlLength,
      "%s%s%s[Telnet] recent RAM replay begins (%llu bytes%s); live output follows\r\n",
      telnetRetainedCrash[0] ? "[retained reset report] " : "",
      telnetRetainedCrash,
      telnetRetainedCrash[0] ? "\r\n" : "",
      static_cast<unsigned long long>(telnetReplayEnd - telnetPosition),
      telnetPosition ? ", older bytes overwritten; first line may be partial" : "");
  if (extra > 0) {
    const size_t room = sizeof(telnetControl) - telnetControlLength - 1;
    telnetControlLength += static_cast<size_t>(extra) < room ? static_cast<size_t>(extra) : room;
  }
  telnetSessionActive = true;
  DebugSerial.println("Telnet client connected");
}

// Each invocation makes at most four nonblocking send attempts of 256 bytes.
// A slow/absent reader can lose old history, but cannot wait on TCP writes here.
static void TelnetDrain() {
  if (!telnetSessionActive) return;
  const int socketFd = telnetClient.fd();
  if (socketFd < 0) return;
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    uint8_t chunk[256];
    size_t size = 0;
    bool control = telnetControlSent < telnetControlLength;
    if (control) {
      size = telnetControlLength - telnetControlSent;
      if (size > sizeof(chunk)) size = sizeof(chunk);
      memcpy(chunk, telnetControl + telnetControlSent, size);
    } else {
      uint64_t oldest;
      portENTER_CRITICAL(&telnetLogMux);
      oldest = telnetHistory.oldest();
      if (telnetPosition >= oldest) {
        size_t limit = sizeof(chunk);
        if (!telnetReplayEnded && telnetPosition < telnetReplayEnd &&
            telnetReplayEnd - telnetPosition < limit) {
          limit = static_cast<size_t>(telnetReplayEnd - telnetPosition);
        }
        size = telnetHistory.copy(telnetPosition, chunk, limit);
      }
      portEXIT_CRITICAL(&telnetLogMux);
      if (telnetPosition < oldest) {
        const uint64_t lost = oldest - telnetPosition;
        telnetPosition = oldest;
        telnetControlLength = snprintf(telnetControl, sizeof(telnetControl),
            "\r\n[Telnet] log overflow: %llu bytes skipped; next line may be partial\r\n",
            static_cast<unsigned long long>(lost));
        telnetControlSent = 0;
        continue;
      }
      if (!telnetReplayEnded && telnetPosition >= telnetReplayEnd) {
        telnetReplayEnded = true;
        static const char replayEnd[] = "\r\n[Telnet] replay end / live output\r\n";
        memcpy(telnetControl, replayEnd, sizeof(replayEnd) - 1);
        telnetControlLength = sizeof(replayEnd) - 1;
        telnetControlSent = 0;
        continue;
      }
    }
    if (!size) return;
    const int sent = send(socketFd, chunk, size, MSG_DONTWAIT);
    if (sent > 0) {
      if (control) telnetControlSent += static_cast<size_t>(sent);
      else telnetPosition += static_cast<size_t>(sent);
    } else {
      if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        telnetClient.stop();
      }
      return;
    }
  }
}

void TelnetRun() {
  if (telnetServer.hasClient()) {
    WiFiClient newClient = telnetServer.available();
    if (telnetSessionActive && telnetClient.connected()) {
      static const char busy[] = "Telnet already connected.\r\n";
      const int otherFd = newClient.fd();
      if (otherFd >= 0) (void)send(otherFd, busy, sizeof(busy) - 1, MSG_DONTWAIT);
      newClient.stop();
    } else {
      telnetClient.stop();
      telnetClient = newClient;
      telnetClient.setNoDelay(true);
      TelnetStartSession();
    }
  }
  if (telnetSessionActive && !telnetClient.connected()) {
    telnetClient.stop();
    telnetSessionActive = false;
    DebugSerial.println("Telnet client disconnected");
  }
  if (!telnetSessionActive) return;
  // This is a read-only diagnostic endpoint, not a command console. Bound
  // incoming data work too; a client must not starve game timers by flooding.
  for (unsigned i = 0; i < 64 && telnetClient.available(); ++i) {
    (void)telnetClient.read();
  }
  TelnetDrain();
}
