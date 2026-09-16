#include "remote_console.h"
#include "remote_console_policy.h"

#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace iotglove {
namespace {
constexpr uint16_t kPort = 23;
constexpr size_t kChunk = 128;
ConsoleByteRing<2048> usbLog;
ConsoleByteRing<6144> telnetLog;
portMUX_TYPE logMutex = portMUX_INITIALIZER_UNLOCKED;
QueueHandle_t commands = nullptr;
std::atomic<bool> listening{false}, connected{false};
std::atomic<uint32_t> rejectedCommands{0};
int firmwareVersion = 0, partitionVersion = 0;
uint32_t bootIdentifier = 0;
bool initialized = false;

size_t takeBytes(bool usb, uint8_t* out, size_t size) {
  portENTER_CRITICAL(&logMutex);
  const size_t count = usb ? usbLog.read(out, size) : telnetLog.read(out, size);
  portEXIT_CRITICAL(&logMutex);
  return count;
}

bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }

void closeSocket(int& descriptor) {
  if (descriptor >= 0) { ::close(descriptor); descriptor = -1; }
}

bool makeNonblocking(int descriptor) {
  const int flags = fcntl(descriptor, F_GETFL, 0);
  return flags >= 0 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) >= 0;
}

int startListener() {
  const int descriptor = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (descriptor < 0) return -1;
  int enabled = 1;
  setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(kPort);
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (!makeNonblocking(descriptor) || bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
      listen(descriptor, 1) < 0) { ::close(descriptor); return -1; }
  return descriptor;
}

void consoleWorker(void*) {
  int listener = -1, client = -1;
  uint32_t lastBind = 0, lastProgress = 0;
  TelnetCommandParser parser;
  ConsoleCommandBudget budget;
  uint8_t outgoing[kChunk];
  size_t pending = 0, offset = 0;
  while (true) {
    const uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) {
      closeSocket(client); closeSocket(listener);
      connected.store(false); listening.store(false);
      pending = offset = 0; parser.reset();
      xQueueReset(commands);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (listener < 0 && (lastBind == 0 || uint32_t(now - lastBind) >= 1000U)) {
      lastBind = now; listener = startListener(); listening.store(listener >= 0);
      if (listener >= 0) remoteConsoleLogf("[remote] ready ip=%s port=23 MAC=%s\n",
          WiFi.localIP().toString().c_str(), WiFi.macAddress().c_str());
    }
    if (listener >= 0) {
      const int accepted = accept(listener, nullptr, nullptr);
      if (accepted >= 0) {
        if (client >= 0 || !makeNonblocking(accepted)) ::close(accepted);
        else {
          client = accepted;
          int enabled = 1;
          setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
          connected.store(true); parser.reset(); pending = offset = 0;
          lastProgress = now;
          remoteConsoleLogf("[remote] connected ip=%s MAC=%s fw=%d partition=%d boot=%lu; command s/?/p/b/u + Enter\n",
              WiFi.localIP().toString().c_str(), WiFi.macAddress().c_str(), firmwareVersion,
              partitionVersion, (unsigned long)bootIdentifier);
          const char status = 's';
          xQueueSend(commands, &status, 0);
        }
      }
    }
    if (client >= 0) {
      uint8_t input[64];
      const int count = recv(client, input, sizeof(input), MSG_DONTWAIT);
      if (count == 0 || (count < 0 && !wouldBlock())) closeSocket(client);
      for (int n = 0; n < count && client >= 0; ++n) {
        char command = 0;
        if (parser.feed(input[n], command)) {
          if (!budget.allow(now) || xQueueSend(commands, &command, 0) != pdTRUE) ++rejectedCommands;
        }
      }
      if (client >= 0) {
        if (offset == pending) { pending = takeBytes(false, outgoing, sizeof(outgoing)); offset = 0; }
        if (pending) {
          const int sent = send(client, outgoing + offset, pending - offset, MSG_DONTWAIT);
          if (sent > 0) { offset += static_cast<size_t>(sent); lastProgress = now; }
          else if ((sent < 0 && !wouldBlock()) || uint32_t(now - lastProgress) >= 5000U) closeSocket(client);
        } else lastProgress = now;
      }
    }
    if (client < 0 && connected.exchange(false)) {
      parser.reset(); pending = offset = 0; xQueueReset(commands);
      remoteConsoleLogf("[remote] client disconnected\n");
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

class SafeWifiPrint : public Print {
 public:
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* bytes, size_t size) override {
    for (size_t n = 0; n < size; ++n) {
      const char* safe = filter_.feed(bytes[n]);
      if (safe) remoteConsoleWrite(reinterpret_cast<const uint8_t*>(safe), strlen(safe));
    }
    return size;
  }
 private:
  WifiLifecycleFilter filter_;
};
SafeWifiPrint wifiPrint;
}  // namespace

void remoteConsoleWrite(const uint8_t* bytes, size_t size) {
  if (!bytes) return;
  // All production messages fit this bound. One short lock preserves complete
  // records across producers; no lock is held during socket or serial I/O.
  if (size > 1536U) { bytes += size - 1536U; size = 1536U; }
  portENTER_CRITICAL(&logMutex);
  usbLog.write(bytes, size); telnetLog.write(bytes, size);
  portEXIT_CRITICAL(&logMutex);
}

void remoteConsoleLogf(const char* format, ...) {
  char text[384];
  va_list args; va_start(args, format);
  const int result = vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (result <= 0) return;
  const size_t count = static_cast<size_t>(result) < sizeof(text) ? static_cast<size_t>(result) : sizeof(text) - 1;
  remoteConsoleWrite(reinterpret_cast<const uint8_t*>(text), count);
}

bool remoteConsoleBegin(int firmware, int partition, uint32_t bootId, bool enableNetwork) {
  if (initialized) return true;
  firmwareVersion = firmware; partitionVersion = partition; bootIdentifier = bootId;
  if (!enableNetwork) { initialized = true; return true; }
  commands = xQueueCreate(8, sizeof(char));
  if (!commands) return false;
  if (xTaskCreatePinnedToCore(consoleWorker, "glove_console", 4096, nullptr, 1, nullptr, 0) != pdPASS) {
    vQueueDelete(commands); commands = nullptr; return false;
  }
  initialized = true;
  return true;
}

void remoteConsolePoll() {
  const int available = Serial.availableForWrite();
  if (available <= 0) return;
  uint8_t bytes[64];
  const size_t capacity = static_cast<size_t>(available) < sizeof(bytes) ? static_cast<size_t>(available) : sizeof(bytes);
  const size_t count = takeBytes(true, bytes, capacity);
  if (count) Serial.write(bytes, count);
}

bool remoteConsoleReadCommand(char& command) { return commands && xQueueReceive(commands, &command, 0) == pdTRUE; }
Print& remoteConsoleWifiPrint() { return wifiPrint; }
RemoteConsoleStatus remoteConsoleStatus() {
  RemoteConsoleStatus status;
  status.listening = listening.load(); status.connected = connected.load();
  status.rejectedCommands = rejectedCommands.load();
  portENTER_CRITICAL(&logMutex);
  status.usbDroppedBytes = usbLog.dropped(); status.telnetDroppedBytes = telnetLog.dropped();
  portEXIT_CRITICAL(&logMutex);
  return status;
}

}  // namespace iotglove
