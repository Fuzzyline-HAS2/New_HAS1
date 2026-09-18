#pragma once
#include "game_state.h"
#include "state_policy.h"
#include <string.h>

namespace iotglove {
inline bool sameEventSession(const GameEvent& e, const ServerSnapshot& s) {
  return s.valid && gameMutationsAllowed(s) &&
         strcmp(e.session, s.session) == 0 && strcmp(e.deviceName, s.deviceName) == 0;
}
inline bool commandAllowed(const GameEvent& e, const ServerSnapshot& s) {
  if (!sameEventSession(e, s)) return false;
  switch (e.kind) {
    case GameEvent::Kind::Capture: return s.capturesAllowed && s.role == Role::Player && s.lifeChip == 1;
    case GameEvent::Kind::CancelCapture:
      return s.role == Role::Ghost && !s.sacrificed && !s.open && s.lifeChip == 1;
    case GameEvent::Kind::Revive:
      return s.role == Role::Ghost && s.sacrificed && s.open &&
             s.revivalCount == 4 && s.lifeChip == 1;
    case GameEvent::Kind::ChipInserted: return s.role == Role::Ghost && s.lifeChip == 0;
    case GameEvent::Kind::ChipRemoved: return s.role == Role::Ghost && s.lifeChip == 1;
    case GameEvent::Kind::SetCount: return s.role == Role::Ghost && e.value <= (s.sacrificed ? 4 : 3);
  }
  return false;
}
inline bool commandApplied(const GameEvent& e, const ServerSnapshot& s) {
  if (!sameEventSession(e, s)) return false;
  switch (e.kind) {
    case GameEvent::Kind::Capture: return s.role == Role::Ghost && s.lifeChip == 0;
    case GameEvent::Kind::CancelCapture:
    case GameEvent::Kind::Revive: return s.role == Role::Player && s.lifeChip == 1;
    case GameEvent::Kind::ChipInserted: return s.role == Role::Ghost && s.lifeChip == 1;
    case GameEvent::Kind::ChipRemoved: return s.role == Role::Ghost && s.lifeChip == 0;
    case GameEvent::Kind::SetCount: return s.role == Role::Ghost && s.revivalCount == e.value;
  }
  return false;
}
}  // namespace iotglove
