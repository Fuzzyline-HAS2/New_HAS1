#pragma once

#include <stdint.h>

// Generator default: BLE off. Do not link the beacon library when off.
// Its strong bleInUse() retains Bluetooth memory even without begin().
#ifndef HAS1_GENERATOR_BLE_ENABLED
#define HAS1_GENERATOR_BLE_ENABLED 0
#endif
#if HAS1_GENERATOR_BLE_ENABLED != 0 && HAS1_GENERATOR_BLE_ENABLED != 1
#error "HAS1_GENERATOR_BLE_ENABLED must be 0 or 1"
#endif

#if HAS1_GENERATOR_BLE_ENABLED
#include <HAS1BleBeacon.h>
#endif

namespace GeneratorBle {
constexpr bool enabled() { return HAS1_GENERATOR_BLE_ENABLED != 0; }
constexpr const char* mode() { return enabled() ? "enabled" : "disabled"; }
constexpr const char* bootMessage() {
  return enabled() ? "[BLE] enabled; controller startup requested"
                   : "[BLE] disabled; controller startup excluded";
}

#if HAS1_GENERATOR_BLE_ENABLED
using Diagnostics = Has1BleBeacon::Diagnostics;
inline void begin() { Has1BleBeacon::begin(); }
inline void poll() { Has1BleBeacon::poll(); }
inline void setDeviceName(const char* name) { Has1BleBeacon::setDeviceName(name); }
inline void suspend() { Has1BleBeacon::suspend(); }
inline bool releaseMemory() { return Has1BleBeacon::releaseMemory(); }
inline Diagnostics diagnostics() { return Has1BleBeacon::diagnostics(); }
#else
// Same diagnostic field names, without importing any Bluetooth library header.
// Step 0 means unavailable; radio 1 means off in the existing Telnet encoding.
struct Diagnostics {
  uint8_t step = 0, radio = 1, lastError = 0, hciStatus = 0;
  uint16_t pendingOpcode = 0;
  bool awaitingCompletion = false, completionTimedOut = false;
  uint32_t sentCommands = 0, failures = 0;
};
inline void begin() {}
inline void poll() {}
inline void setDeviceName(const char*) {}
inline void suspend() {}
// The controller never started; do not stop it or release its memory twice.
inline bool releaseMemory() { return false; }
inline Diagnostics diagnostics() { return {}; }
#endif
}  // namespace GeneratorBle
