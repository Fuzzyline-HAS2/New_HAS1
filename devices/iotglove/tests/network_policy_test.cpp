#include "network_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
using namespace iotglove;
int main() {
  ServerSnapshot s;
  s.valid = true; s.phase = Phase::Active; s.role = Role::Player;
  strcpy(s.deviceName, "G1P3"); strcpy(s.session, "boot-1");
  GameEvent e;
  strcpy(e.deviceName, s.deviceName); strcpy(e.session, s.session);
  e.kind = GameEvent::Kind::SetCount; e.value = 1;
  assert(!commandAllowed(e, s));
  s.role = Role::Tagger; assert(!commandAllowed(e, s));
  s.role = Role::Neutral; assert(!commandAllowed(e, s));
  s.role = Role::Ghost;
  assert(commandAllowed(e, s)); assert(!commandApplied(e, s));
  s.revivalCount = 1; assert(commandApplied(e, s));
  e.value = 3; assert(commandAllowed(e, s));
  e.value = 4; assert(!commandAllowed(e, s));
  s.sacrificed = true; assert(commandAllowed(e, s));
  e.value = 5; assert(!commandAllowed(e, s));
  e.value = 0; assert(commandAllowed(e, s));
  s.phase = Phase::Ready; assert(!commandAllowed(e, s));
  s.phase = Phase::Active; s.deviceState = DeviceState::Setting;
  assert(!commandAllowed(e, s));
  s.deviceState = DeviceState::Other; s.valid = false;
  assert(!commandAllowed(e, s)); s.valid = true;
  strcpy(s.session, "boot-2");
  assert(!commandAllowed(e, s)); assert(!commandApplied(e, s));
  strcpy(s.session, e.session); strcpy(s.deviceName, "G1P4");
  assert(!commandAllowed(e, s));
  strcpy(s.deviceName, e.deviceName);
  e.kind = static_cast<GameEvent::Kind>(255); assert(!commandAllowed(e, s));
  puts("PASS: count-only commands, authoritative ghost role and session/freshness gates");
}
