#include "../game_state.h"
#include "../feedback.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>

using namespace iotglove;

static ServerSnapshot snapshot(Role role = Role::Ghost) {
  ServerSnapshot s;
  s.valid = true;
  strcpy(s.session, "G1:run1");
  strcpy(s.deviceName, "G1P1");
  s.phase = Phase::Active;
  s.role = role;
  s.stepSeconds = 3;
  s.capturesAllowed = true;
  return s;
}

static GameEvent pop(GameModel& game, GameEvent::Kind kind, int value = -1) {
  GameEvent event;
  assert(game.peekEvent(event));
  assert(event.kind == kind);
  if (value >= 0) assert(event.value == value);
  assert(strcmp(event.deviceName, "G1P1") == 0);
  game.consumeEvent();
  return event;
}

static void noEvent(GameModel& game) {
  GameEvent event;
  assert(!game.peekEvent(event));
}

static void trainingBoundaries() {
  GameModel g(Profile::Training);
  g.begin(true, 100);
  g.tick(100);
  assert(g.feedback().display == Display::Player);
  g.chipChanged(false, 200);
  Feedback f = g.feedback();
  assert(f.display == Display::Ghost && f.lit == 1 && f.haptic == Haptic::Removed);
  g.tick(3199); assert(g.count() == 1);
  g.tick(3200); assert(g.count() == 2);
  g.chipChanged(true, 5000);
  g.tick(6200); assert(g.count() == 3);
  g.tick(9199); assert(g.feedback().display == Display::Ghost);
  g.tick(9200); assert(g.feedback().display == Display::Player);
  noEvent(g);
  g.chipChanged(false, 10000);
  g.tick(19000); assert(g.count() == 4 && g.feedback().display == Display::Ghost);
  g.chipChanged(true, 19001); assert(g.feedback().display == Display::Player);
}

static void trainingResetAndBoot() {
  GameModel g(Profile::Training);
  g.begin(false, UINT32_MAX - 1000U);
  g.tick(UINT32_MAX - 1000U);
  assert(g.feedback().display == Display::Ghost && g.count() == 1);
  g.tick(1999); assert(g.count() == 2);  // unsigned millis rollover
  g.chipChanged(true, 2500);
  g.buttonPressed(2600);
  assert(g.feedback().haptic == Haptic::Found);
  g.tick(11599); assert(g.feedback().display == Display::Ghost);
  g.tick(11600); assert(g.feedback().display == Display::Player);
  g.buttonPressed(12000); assert(g.feedback().haptic == Haptic::None);
  g.chipChanged(false, 13000);
  g.chipChanged(true, 15000);
  g.chipChanged(false, 16000);
  g.tick(19000); assert(g.count() == 2);
}

static void chipChangesNeverRequestRolesOrLife() {
  for (Role role : {Role::Neutral, Role::Player, Role::Ghost, Role::Tagger}) {
    for (DeviceState device : {DeviceState::Other, DeviceState::Setting, DeviceState::Ready,
         DeviceState::Blink, DeviceState::Activate, DeviceState::Exploration, DeviceState::Ended}) {
      GameModel g(Profile::Origin);
      g.begin(true, 0);
      auto s = snapshot(role); s.deviceState = device; s.lifeChip = 1;
      g.applyServer(s, 10);
      g.chipChanged(false, 20);
      assert(!g.chipPresent() && g.server().role == role && g.server().lifeChip == 1);
      assert(g.feedback().haptic == Haptic::None); noEvent(g);
      g.chipChanged(true, 30);
      assert(g.chipPresent() && g.server().role == role && g.server().lifeChip == 1);
      assert(g.feedback().haptic == Haptic::None); noEvent(g);
      s.valid = false; g.applyServer(s, 40);
      g.chipChanged(false, 50);
      assert(!g.chipPresent()); noEvent(g);  // Observation still updates while offline.
    }
  }
}

static void playerColorWaitsForAuthoritativeRole() {
  GameModel g(Profile::Origin);
  FeedbackEngine engine;
  g.begin(true, 0);
  auto s = snapshot(Role::Player);
  s.deviceState = DeviceState::Activate;
  g.applyServer(s, 10);
  auto out = engine.update(g.feedback(), 0, false, 10);
  assert(out.red == 0 && out.green == 64 && out.blue == 0 && out.lit == 4);
  g.chipChanged(false, 20);
  noEvent(g);
  out = engine.update(g.feedback(), 0, false, 20);
  assert(out.red == 0 && out.green == 64 && out.blue == 0 && out.lit == 4);
  g.applyServer(s, 30);  // Physical removal cannot override the server role.
  out = engine.update(g.feedback(), 0, false, 30);
  assert(out.red == 0 && out.green == 64 && out.blue == 0 && out.lit == 4);
  g.chipChanged(true, 40);
  out = engine.update(g.feedback(), 0, false, 40);
  assert(out.green == 64 && out.blue == 0 && out.lit == 4);
  s.role = Role::Ghost; s.revivalCount = 2;
  g.applyServer(s, 50);
  out = engine.update(g.feedback(), 0, false, 50);
  assert(out.red == 0 && out.green == 0 && out.blue == 64 && out.lit == 2);
  noEvent(g);
  g.chipChanged(false, 60); noEvent(g);
  s.role = Role::Player; g.applyServer(s, 70);
  out = engine.update(g.feedback(), 0, false, 70);
  assert(out.red == 0 && out.green == 64 && out.blue == 0 && out.lit == 4);
  noEvent(g);  // Server revival while chip is absent never triggers client recapture.
}

static void countAndServerRevival() {
  GameModel g(Profile::Origin);
  g.begin(false, 0);
  auto s = snapshot();
  g.applyServer(s, 100);
  g.tick(3099); assert(g.count() == 0);
  g.tick(3100); assert(g.count() == 1);
  pop(g, GameEvent::Kind::SetCount, 1);
  g.applyServer(s, 3500); assert(g.count() == 1);  // old read does not roll back
  s.revivalCount = 1;
  g.applyServer(s, 4000);  // ack must not move 6100 boundary
  g.tick(6100); assert(g.count() == 2);
  pop(g, GameEvent::Kind::SetCount, 2);
  g.tick(15100); assert(g.count() == 3);  // pending sacrifice cap
  pop(g, GameEvent::Kind::SetCount, 3);
  assert(!g.canUseLifeDevice());
  s.revivalCount = 3; s.sacrificed = true;
  g.applyServer(s, 15110);
  g.tick(18100); assert(g.count() == 4);
  pop(g, GameEvent::Kind::SetCount, 4);
  assert(g.canUseLifeDevice());
  g.chipChanged(true, 18200);
  noEvent(g);  // Physical observation does not mutate life or role.
  s.revivalCount = 4; s.open = true;
  g.applyServer(s, 18300);
  g.tick(18300);
  noEvent(g);  // Even all revival conditions wait for a server role transition.
  assert(g.feedback().display == Display::Ghost && !g.canUseLifeDevice());
  s.role = Role::Player; g.applyServer(s, 18400);
  assert(g.feedback().display == Display::Player);
  noEvent(g);
}

static void foundRetainsChipAndOpen() {
  GameModel g(Profile::Origin);
  g.begin(false, 0);
  auto s = snapshot(); s.sacrificed = true; s.open = true; s.revivalCount = 2;
  g.applyServer(s, 100);
  g.chipChanged(true, 200); noEvent(g);
  g.buttonPressed(300); pop(g, GameEvent::Kind::SetCount, 0);
  assert(g.chipPresent() && !g.canUseLifeDevice());
  s.revivalCount = 0; g.applyServer(s, 500);
  g.tick(12300); pop(g, GameEvent::Kind::SetCount, 4);
  noEvent(g);  // Automatic revival belongs to the server.
}

static void rebootRestoreAndPendingCountOrder() {
  GameModel g(Profile::Origin);
  g.begin(true, 0);
  auto s = snapshot();
  s.open = true; s.sacrificed = true; s.lifeChip = 1; s.revivalCount = 3;
  g.applyServer(s, 100);
  noEvent(g);  // no new physical insertion delta after reboot
  g.tick(3100); pop(g, GameEvent::Kind::SetCount, 4);
  noEvent(g);  // Automatic revival belongs to the server.

  GameModel resetting(Profile::Origin);
  resetting.begin(false, 0);
  s.open = false; s.lifeChip = 0; s.revivalCount = 0;
  resetting.applyServer(s, 0);
  resetting.tick(3000); pop(resetting, GameEvent::Kind::SetCount, 1);
  resetting.buttonPressed(3100); pop(resetting, GameEvent::Kind::SetCount, 0);
  s.revivalCount = 1; resetting.applyServer(s, 3200);
  assert(resetting.count() == 0);  // earlier in-flight count ack cannot undo button
  s.revivalCount = 0; resetting.applyServer(s, 3300);
  resetting.tick(6099); assert(resetting.count() == 0);
  resetting.tick(6100); pop(resetting, GameEvent::Kind::SetCount, 1);
}

static void roleChangeDropsQueuedCounts() {
  GameModel g(Profile::Origin); g.begin(false, 0);
  auto s = snapshot(Role::Ghost); g.applyServer(s, 0);
  g.tick(3000);
  GameEvent count; assert(g.peekEvent(count) && count.value == 1);
  s.role = Role::Player; g.applyServer(s, 3100);
  noEvent(g); assert(g.count() == 0);
  g.tick(10000); g.buttonPressed(10001); noEvent(g);
  s.role = Role::Ghost; g.applyServer(s, 11000);
  g.tick(13999); noEvent(g);
  g.tick(14000); pop(g, GameEvent::Kind::SetCount, 1);
}

static void remoteChangesAndFailures() {
  GameModel g(Profile::Origin);
  g.begin(false, UINT32_MAX - 1000U);
  auto s = snapshot(); s.sacrificed = true;
  g.applyServer(s, UINT32_MAX - 1000U);
  g.tick(1999); pop(g, GameEvent::Kind::SetCount, 1);
  s.revivalCount = 1; g.applyServer(s, 2000);
  s.stepSeconds = 1; g.applyServer(s, 2500);
  g.tick(3499); assert(g.count() == 1);
  g.tick(3500); auto event = pop(g, GameEvent::Kind::SetCount, 2);
  g.commandUncertain(event.sequence);
  assert(!g.synchronized());
  g.buttonPressed(3600); noEvent(g);
  g.applyServer(s, 3700); assert(g.count() == 1 && g.synchronized());
  g.buttonPressed(3800);
  strcpy(s.session, "G1:run2");
  s.phase = Phase::Exploration;
  g.applyServer(s, 3900);
  noEvent(g); assert(g.feedback().display == Display::Ready);
  g.chipChanged(true, 4000); noEvent(g);
  s.phase = Phase::Ended; s.role = Role::Tagger;
  g.applyServer(s, 4100); assert(g.feedback().display == Display::Ended);
  s.valid = false;
  g.applyServer(s, 4101);
  assert(!g.synchronized() && !g.server().valid);
  g.chipChanged(false, 4102); noEvent(g);
}

static void debounceAndOverflow() {
  DebouncedInput input;
  input.begin(false, UINT32_MAX - 20U);
  assert(!input.update(true, UINT32_MAX - 10U));
  assert(!input.update(false, UINT32_MAX - 5U));
  assert(!input.update(true, 0));
  assert(!input.update(true, 29));
  assert(input.update(true, 30));
  assert(!input.update(true, 100));
  GameModel g(Profile::Origin);
  g.begin(false, 0);
  auto s = snapshot(); s.sacrificed = true;
  g.applyServer(s, 0);
  for (unsigned n = 1; n < 15; ++n) g.buttonPressed(n);
  assert(g.queueOverflowed() && !g.synchronized());
  noEvent(g);
  g.applyServer(s, 100);
  assert(g.synchronized() && !g.queueOverflowed());
  noEvent(g);
}

static void explicitPreparationStates() {
  for (Role role : {Role::Neutral, Role::Player, Role::Ghost, Role::Tagger}) {
    for (Phase phase : {Phase::Setting, Phase::Ready, Phase::Active}) {
      for (DeviceState device : {DeviceState::Setting, DeviceState::Ready}) {
        GameModel g(Profile::Origin);
        g.begin(false, 0);
        auto s = snapshot(role);
        s.phase = phase; s.deviceState = device; s.connectionEpoch = 7;
        s.lifeChip = 1; s.sacrificed = true; s.open = true;
        g.applyServer(s, 10);
        auto f = g.feedback();
        assert(f.display == (device == DeviceState::Setting ? Display::Setting : Display::Ready));
        assert(f.lit == 3 && f.stateValid && f.stateEpoch == 7);
        assert(f.role == role && f.phase == phase && f.deviceState == device);
        g.chipChanged(true, 20);
        assert(g.feedback().lit == 4);
        g.chipChanged(false, 30);
        g.buttonPressed(40); g.tick(10000);
        assert(g.feedback().lit == 3 && g.count() == 0);
        noEvent(g);  // Preparation edges/timers never mutate life_chip or role.
      }
    }
  }
  // A display change out of live play discards an unsubmitted count safely.
  GameModel g(Profile::Origin); g.begin(false, 0);
  auto s = snapshot(Role::Ghost); s.deviceState = DeviceState::Activate;
  g.applyServer(s, 0); g.tick(3000);
  GameEvent count; assert(g.peekEvent(count) && count.kind == GameEvent::Kind::SetCount);
  s.deviceState = DeviceState::Ready; g.applyServer(s, 3010);
  noEvent(g); assert(g.feedback().haptic == Haptic::None);
  s.deviceState = DeviceState::Activate; g.applyServer(s, 3020);
  noEvent(g);  // Returning to active cannot replay the earlier queued count.
}

static void taggerDisplayAndSafety() {
  for (Phase phase : {Phase::Setting, Phase::Ready, Phase::Active}) {
    GameModel g(Profile::Origin); g.begin(false, 0);
    auto s = snapshot(Role::Tagger); s.phase = phase;
    s.deviceState = DeviceState::Blink; s.connectionEpoch = 1;
    g.applyServer(s, 10);
    auto f = g.feedback();
    assert(f.display == Display::TaggerBlink && f.lit == 4 && f.stateValid);
    s.deviceState = DeviceState::Activate; g.applyServer(s, 20);
    f = g.feedback(); assert(f.display == Display::TaggerActive && f.stateEpoch == 1);
    s.deviceState = DeviceState::Other; g.applyServer(s, 30);
    assert(g.feedback().display != Display::TaggerBlink);  // Unknown is not blink.
    noEvent(g);
  }
  GameModel g(Profile::Origin); g.begin(true, 0);
  auto s = snapshot(Role::Tagger); s.deviceState = DeviceState::Blink;
  s.connectionEpoch = 9;
  g.applyServer(s, 0); assert(g.feedback().display == Display::TaggerBlink);
  s.valid = false; g.applyServer(s, 10);
  auto f = g.feedback(); assert(!f.stateValid && f.display == Display::Ready);
  s.valid = true; s.connectionEpoch = 10; g.applyServer(s, 20);
  f = g.feedback(); assert(f.stateValid && f.stateEpoch == 10);
  for (Phase phase : {Phase::Exploration, Phase::Ended}) {
    s.phase = phase; g.applyServer(s, 30);
    f = g.feedback();
    assert(f.display == (phase == Phase::Ended ? Display::Ended : Display::Ready));
    g.chipChanged(false, 40); g.buttonPressed(50); g.tick(10000); noEvent(g);
  }
}

int main() {
  trainingBoundaries();
  trainingResetAndBoot();
  chipChangesNeverRequestRolesOrLife();
  playerColorWaitsForAuthoritativeRole();
  countAndServerRevival();
  roleChangeDropsQueuedCounts();
  foundRetainsChipAndOpen();
  rebootRestoreAndPendingCountOrder();
  remoteChangesAndFailures();
  debounceAndOverflow();
  explicitPreparationStates();
  taggerDisplayAndSafety();
  puts("PASS: production game model, debounce, training, count, reconciliation and rollover");
}
