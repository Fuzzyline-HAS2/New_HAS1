#include "beetle.h"
#include <IoTGloveDiagnostics.h>

namespace beetle {
namespace {
iotglove::diagnostics::LogQueue logQueue;
portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t bootResetReason = 0;
uint32_t nextLogSequence = 2;
}

void diagnosticsInit(uint32_t resetReason) { bootResetReason = resetReason; }

void queueBootLog() {
  iotglove::diagnostics::Log log;
  log.sequence = 1;  // Snapshot identity stays fixed, so TTGO can deduplicate.
  log.bootId = bootId;
  log.value = bootResetReason;
  portENTER_CRITICAL(&logMux);
  logQueue.push(log);
  portEXIT_CRITICAL(&logMux);
}

void queueDiagnosticLog(iotglove::diagnostics::Event event,
                        iotglove::diagnostics::Code code, uint32_t value) {
  iotglove::diagnostics::Log log;
  log.bootId = bootId;
  log.uptimeMs = millis();
  log.event = event;
  log.code = code;
  log.value = value;
  portENTER_CRITICAL(&logMux);
  logQueue.push(log);
  portEXIT_CRITICAL(&logMux);
}

void diagnosticsPoll() {
  // Main loop exclusively owns UART writes; worker callbacks only queue enums.
  for (size_t i = 0; i < iotglove::diagnostics::LogQueue::kDrainBudget; ++i) {
    iotglove::diagnostics::Log log;
    portENTER_CRITICAL(&logMux);
    const bool haveLog = logQueue.pop(log);
    portEXIT_CRITICAL(&logMux);
    if (!haveLog) return;
    if (!log.sequence) {
      log.sequence = nextLogSequence++;
      if (nextLogSequence < 2) nextLogSequence = 2;
    }
    iotglove::wire::Frame frame;
    if (iotglove::diagnostics::makeLogFrame(log, frame)) sendFrame(frame);
  }
}
}
