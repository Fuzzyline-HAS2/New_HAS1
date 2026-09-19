#include "HAS1BleBeacon.h"

#include <Arduino.h>
#include <esp_bt.h>
#include <esp32-hal-bt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

// Arduino ESP32 3.3.11 releases unused controller memory before setup(). Keep
// only BLE memory: the legacy btInUse() override would retain Classic as well.
extern "C" bool bleInUse(void) { return true; }

namespace Has1BleBeacon {
namespace {
StateMachine machine;
bool startupAttempted = false;
bool controllerAvailable = false;

// Xtensa's toolchain does not promise lock-free C++ atomics. Use the ESP-IDF
// cross-core critical-section primitive instead. Every scope below copies only
// fixed primitive fields: no controller calls, parsing, timing, or FSM work.
// SAFE selects the task/ISR entry point appropriate to the callback context.
portMUX_TYPE mailboxMux = portMUX_INITIALIZER_UNLOCKED;
uint16_t expectedOpcode = 0;
struct Completion {
  uint32_t receivedAt;
  uint16_t opcode;
  uint8_t status;
  bool ready;
};
Completion mailbox = {};

void sendAvailable() {}

int receive(uint8_t* data, uint16_t size) {
  const uint32_t receivedAt = static_cast<uint32_t>(millis());
  uint16_t opcode = 0;
  uint8_t status = 0;
  if (!decodeCommandEvent(data, size, opcode, status) || opcode == 0) return 0;
  portENTER_CRITICAL_SAFE(&mailboxMux);
  if (expectedOpcode == opcode && !mailbox.ready) {
    expectedOpcode = 0;  // A duplicate final response cannot claim it again.
    mailbox.receivedAt = receivedAt;
    mailbox.opcode = opcode;
    mailbox.status = status;
    mailbox.ready = true;
  }
  portEXIT_CRITICAL_SAFE(&mailboxMux);
  return 0;
}

esp_vhci_host_callback_t callbacks = {sendAvailable, receive};
}  // namespace

void begin() {
  if (startupAttempted) return;
  startupAttempted = true;
  controllerAvailable = btStartMode(BT_MODE_BLE) &&
      esp_vhci_host_register_callback(&callbacks) == ESP_OK;
  machine.start(controllerAvailable);
}

void setDeviceName(const char* name) { machine.setDeviceName(name); }

void poll(bool allowRetry) {
  if (!controllerAvailable) return;
  Completion completion;
  portENTER_CRITICAL_SAFE(&mailboxMux);
  completion = mailbox;
  mailbox.ready = false;
  portEXIT_CRITICAL_SAFE(&mailboxMux);
  if (completion.ready) {
    machine.complete(completion.opcode, completion.status, completion.receivedAt);
  }

  const Diagnostics state = machine.diagnostics();
  const bool canSend = allowRetry && controllerAvailable &&
      state.step != Step::Unavailable && !state.awaitingCompletion &&
      esp_vhci_host_check_send_available();
  const Command* command = machine.poll(static_cast<uint32_t>(millis()), allowRetry,
                                        canSend);
  if (command == nullptr) return;
  // Mark ownership before send: a synchronous/very fast controller callback is
  // allowed. No critical section is held around the Espressif send function.
  portENTER_CRITICAL_SAFE(&mailboxMux);
  expectedOpcode = command->opcode;
  portEXIT_CRITICAL_SAFE(&mailboxMux);
  esp_vhci_host_send_packet(const_cast<uint8_t*>(command->data), command->size);
}

Diagnostics diagnostics() { return machine.diagnostics(); }

}  // namespace Has1BleBeacon
