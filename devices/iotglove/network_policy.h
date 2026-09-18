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
  return sameEventSession(e, s) && e.kind == GameEvent::Kind::SetCount &&
         s.role == Role::Ghost && e.value <= (s.sacrificed ? 4 : 3);
}
inline bool commandApplied(const GameEvent& e, const ServerSnapshot& s) {
  return sameEventSession(e, s) && e.kind == GameEvent::Kind::SetCount &&
         s.role == Role::Ghost && s.revivalCount == e.value;
}
}  // namespace iotglove
