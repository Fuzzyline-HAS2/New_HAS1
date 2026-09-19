#pragma once

#include "BeaconState.h"

namespace Has1BleBeacon {

// Call once in setup after required peripheral/Wi-Fi initialization, before
// starting actuators or safety-critical elapsed-time deadlines.
// This boot-only call may block inside the Arduino controller startup routine.
// A startup failure disables BLE until reboot; poll never restarts the controller.
void begin();

// These APIs must share one caller (the Arduino loop task). setDeviceName may
// precede begin. Neither API waits for HCI responses or starts the controller.
void setDeviceName(const char* name);
void poll(bool allowRetry = true);
Diagnostics diagnostics();

}  // namespace Has1BleBeacon
