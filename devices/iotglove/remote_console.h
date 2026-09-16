#pragma once

#include <stddef.h>
#include <stdint.h>

class Print;

namespace iotglove {

struct RemoteConsoleStatus {
  bool listening = false;
  bool connected = false;
  uint32_t usbDroppedBytes = 0;
  uint32_t telnetDroppedBytes = 0;
  uint32_t rejectedCommands = 0;
};

bool remoteConsoleBegin(int firmware, int partition, uint32_t bootId, bool enableNetwork);
void remoteConsolePoll();  // Nonblocking USB drain, called by the application loop.
bool remoteConsoleReadCommand(char& command);
void remoteConsoleWrite(const uint8_t* bytes, size_t size);  // Thread-safe bounded fan-out.
void remoteConsoleLogf(const char* format, ...);
Print& remoteConsoleWifiPrint();  // Emits fixed Wi-Fi lifecycle messages, never raw credentials/HTTP payloads.
RemoteConsoleStatus remoteConsoleStatus();

}  // namespace iotglove
