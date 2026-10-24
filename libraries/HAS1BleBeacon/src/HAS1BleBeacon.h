#pragma once

#include "BeaconState.h"

namespace Has1BleBeacon {

// Call once in setup after required peripheral/Wi-Fi initialization, before
// starting actuators or safety-critical elapsed-time deadlines.
// This boot-only call may block inside the Arduino controller startup routine.
// A startup failure disables BLE until reboot or a later resume(); poll never
// restarts the controller.
void begin();

// Maintenance only (generator GitHub OTA): stops advertising (waiting up to
// 500 ms for HCI), then disables and deinitializes the controller so its heap
// returns for TLS. poll() does nothing until resume(). No effect before begin().
void suspend();
// After a suspend() that stopped the controller: returns its static memory to
// the heap for the rest of this boot (Arduino btMemRelease). BLE then starts
// only after a reboot; resume() reports StartupFailed. Returns false when the
// controller did not stop or the release failed.
bool releaseMemory();
// After suspend(): restarts the controller and replays the full advertising
// setup for the current name. A failed restart is reported as StartupFailed and
// leaves BLE off until the next suspend/resume or reboot.
void resume();

// These APIs must share one caller (the Arduino loop task). setDeviceName may
// precede begin. Neither API waits for HCI responses or starts the controller.
void setDeviceName(const char* name);
void poll(bool allowRetry = true);
Diagnostics diagnostics();

}  // namespace Has1BleBeacon
