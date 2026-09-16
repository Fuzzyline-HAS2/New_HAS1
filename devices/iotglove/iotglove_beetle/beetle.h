#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>
#include <IoTGloveProtocol.h>
#include <IoTGloveLocation.h>
#include <IoTGloveDiagnostics.h>
#include <atomic>
#include <esp_task_wdt.h>
#include "hardware_config.h"
#include "beacon_map.h"

namespace beetle {
extern const int kFirmwareVersion;
extern const int kPartitionVersion;
extern HardwareSerial link;
extern std::atomic<bool> otaBusy;
extern bool scanRequested;
extern uint32_t lastTtgoFrame;
extern uint32_t bootId;

void sendFrame(const iotglove::wire::Frame& frame);
void sendHello(uint32_t requestId = 0);
void sendHeartbeat(uint32_t requestId = 0);
void sendOtaResult(uint32_t requestId, const char* result);
void uartPoll(uint32_t now);
void bleInit();
void blePoll(uint32_t now, bool enabled);
bool bleHealthy(uint32_t now);
iotglove::Location currentLocation(uint32_t now);
void otaInit();
void otaRequest(uint32_t requestId, uint32_t targetVersion = 0);
void otaPoll(uint32_t now);
void otaReplayResult();
bool otaHealthy(uint32_t now);
void diagnosticsInit(uint32_t resetReason);
void queueBootLog();
void queueDiagnosticLog(iotglove::diagnostics::Event event,
                        iotglove::diagnostics::Code code, uint32_t value);
void diagnosticsPoll();
}
