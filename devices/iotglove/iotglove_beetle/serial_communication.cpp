#include "beetle.h"
#include <IoTGloveOta.h>

namespace beetle {
static iotglove::wire::Decoder decoder;

static void putNumber(iotglove::wire::Frame& frame, uint32_t value) {
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%lu", static_cast<unsigned long>(value));
  iotglove::wire::put(frame, buffer);
}

void sendFrame(const iotglove::wire::Frame& frame) {
  char line[iotglove::wire::kMaxLine + 1];
  const size_t size = iotglove::wire::format(line, sizeof(line), frame);
  if (size) link.write(reinterpret_cast<const uint8_t*>(line), size);
}

void sendHello(uint32_t requestId) {
  iotglove::wire::Frame frame;
  strcpy(frame.type, "HELLO");
  frame.id = requestId;
  iotglove::wire::put(frame, "beetle");
  putNumber(frame, kFirmwareVersion);
  putNumber(frame, kPartitionVersion);
  putNumber(frame, bootId);
  sendFrame(frame);
}

void sendHeartbeat(uint32_t requestId) {
  iotglove::wire::Frame frame;
  strcpy(frame.type, "HEART");
  frame.id = requestId;
  putNumber(frame, millis());
  const bool enabled = scanRequested && !otaBusy.load() &&
      millis() - lastTtgoFrame < beetle_config::kTtgoTimeoutMs;
  iotglove::wire::put(frame, enabled ? "1" : "0");
  iotglove::wire::put(frame, otaBusy.load() ? "1" : "0");
  sendFrame(frame);
}

void sendOtaResult(uint32_t requestId, const char* result) {
  iotglove::wire::Frame frame;
  strcpy(frame.type, "OTA_RESULT");
  frame.id = requestId;
  iotglove::wire::put(frame, result);
  putNumber(frame, kFirmwareVersion);
  sendFrame(frame);
}

void uartPoll(uint32_t now) {
  // Per-loop budget prevents garbage UART traffic from starving the reset input.
  for (size_t budget = 0; budget < 256 && link.available(); ++budget) {
    iotglove::wire::Frame frame;
    if (!decoder.feed(static_cast<char>(link.read()), now, frame)) continue;
    if (strcmp(frame.type, "PING") == 0 && frame.count == 0) {
      lastTtgoFrame = now;
      sendHello(frame.id);
      sendHeartbeat(frame.id);
      otaReplayResult();
      queueBootLog();
    } else if (strcmp(frame.type, "MODE") == 0 && frame.count == 2 &&
               (strcmp(frame.args[0], "0") == 0 || strcmp(frame.args[0], "1") == 0) &&
               (strcmp(frame.args[1], "live") == 0 || strcmp(frame.args[1], "training") == 0)) {
      lastTtgoFrame = now;
      scanRequested = strcmp(frame.args[0], "1") == 0;
      sendHeartbeat(frame.id);
    } else if (strcmp(frame.type, "HELLO") == 0 && frame.count == 4 &&
               strcmp(frame.args[0], "ttgo") == 0) {
      uint32_t fw, partition, boot;
      if (iotglove::wire::uint32(frame.args[1], fw) &&
          iotglove::wire::uint32(frame.args[2], partition) &&
          iotglove::wire::uint32(frame.args[3], boot)) {
        lastTtgoFrame = now;
        sendHello(frame.id);
      }
    } else if (strcmp(frame.type, "OTA") == 0 && frame.count == 1 &&
               frame.id != 0 && strcmp(frame.args[0], "check") == 0) {
      lastTtgoFrame = now;
      otaRequest(frame.id);
    } else if (strcmp(frame.type, "OTA") == 0 && frame.count == 2 &&
               frame.id != 0 && strcmp(frame.args[0], "version") == 0) {
      uint32_t targetVersion = 0;
      if (iotglove::ota::parseVersion(frame.args[1], targetVersion)) {
        lastTtgoFrame = now;
        otaRequest(frame.id, targetVersion);
      } else {
        sendOtaResult(frame.id, "failed");
      }
    }
  }
}
}
