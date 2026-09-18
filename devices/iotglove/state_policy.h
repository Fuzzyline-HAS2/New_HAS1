#pragma once

#include "game_state.h"
#include <string.h>

namespace iotglove {

// Device preparation gates game mutations without changing the game phase used
// by OTA/reset safety. Unknown/management commands retain the existing contract.
inline bool gameMutationsAllowed(const ServerSnapshot& state) {
  return state.phase == Phase::Active && state.deviceState != DeviceState::Setting &&
         state.deviceState != DeviceState::Ready && state.deviceState != DeviceState::Exploration &&
         state.deviceState != DeviceState::Ended;
}

inline const char* deviceStateName(DeviceState state) {
  switch (state) {
    case DeviceState::Setting: return "setting";
    case DeviceState::Ready: return "ready";
    case DeviceState::Blink: return "blink";
    case DeviceState::Activate: return "activate";
    case DeviceState::Exploration: return "exploration";
    case DeviceState::Ended: return "ended";
    default: return "other";
  }
}

inline DeviceState decodeDeviceState(const char* state) {
  if (!state) return DeviceState::Other;
  if (!strcmp(state, "setting")) return DeviceState::Setting;
  if (!strcmp(state, "ready")) return DeviceState::Ready;
  if (!strcmp(state, "blink")) return DeviceState::Blink;
  if (!strcmp(state, "activate")) return DeviceState::Activate;
  if (!strcmp(state, "photo") || !strcmp(state, "exploration")) return DeviceState::Exploration;
  if (!strcmp(state, "win") || !strcmp(state, "lose") || !strcmp(state, "stop")) return DeviceState::Ended;
  return DeviceState::Other;
}

// Preserve the existing photo/end overrides; setting/ready are explicit device
// states only, so a live game can never become OTA-safe through a display change.
inline bool decodeServerStates(const char* game, const char* device, ServerSnapshot& out) {
  if (!game || !device) return false;
  if (!strcmp(game, "setting")) out.phase = Phase::Setting;
  else if (!strcmp(game, "ready")) out.phase = Phase::Ready;
  else if (!strcmp(game, "activate")) out.phase = Phase::Active;
  else if (!strcmp(game, "stop") || !strcmp(game, "end")) out.phase = Phase::Ended;
  else return false;
  out.deviceState = decodeDeviceState(device);
  if (out.deviceState == DeviceState::Exploration) out.phase = Phase::Exploration;
  if (out.deviceState == DeviceState::Ended) out.phase = Phase::Ended;
  return true;
}

}  // namespace iotglove
