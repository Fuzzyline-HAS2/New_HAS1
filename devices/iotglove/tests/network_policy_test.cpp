#include "network_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
using namespace iotglove;
int main() {
  ServerSnapshot s;
  s.valid = true; s.phase = Phase::Active; s.role = Role::Player; s.lifeChip = 1;
  s.capturesAllowed = true;
  strcpy(s.deviceName, "G1P3"); strcpy(s.session, "boot-1");
  GameEvent e;
  strcpy(e.deviceName, s.deviceName); strcpy(e.session, s.session);
  e.kind = GameEvent::Kind::Capture;
  assert(commandAllowed(e, s));
  s.capturesAllowed = false; assert(!commandAllowed(e, s));
  s.capturesAllowed = true;
  s.role = Role::Ghost; s.lifeChip = 0;
  assert(!commandAllowed(e, s)); assert(commandApplied(e, s));
  e.kind = GameEvent::Kind::ChipInserted;
  assert(commandAllowed(e, s)); s.lifeChip = 1;
  assert(!commandAllowed(e, s)); assert(commandApplied(e, s));
  e.kind = GameEvent::Kind::CancelCapture;
  assert(commandAllowed(e, s)); s.sacrificed = true;
  assert(!commandAllowed(e, s));
  e.kind = GameEvent::Kind::Revive; s.revivalCount = 4;
  assert(!commandAllowed(e, s)); s.open = true;
  assert(commandAllowed(e, s)); s.revivalCount = 3;
  assert(!commandAllowed(e, s));
  e.kind = GameEvent::Kind::SetCount; e.value = 4;
  assert(commandAllowed(e, s)); s.sacrificed = false;
  assert(!commandAllowed(e, s)); e.value = 0;
  assert(commandAllowed(e, s));
  s.phase = Phase::Ready; assert(!commandAllowed(e, s));
  s.phase = Phase::Active; strcpy(s.session, "boot-2");
  assert(!commandAllowed(e, s)); assert(!commandApplied(e, s));
  puts("network policy tests passed");
}
