#include "../feedback.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace iotglove;
using feedback_config::Pattern;
using feedback_config::Settings;

static Feedback state(Role role, DeviceState device, Phase phase, Display display) {
  Feedback out;
  out.stateValid = true;
  out.role = role;
  out.deviceState = device;
  out.phase = phase;
  out.display = display;
  return out;
}

static void patternsAndConfiguration() {
  Settings config;
  auto pattern = feedback_config::schedule(Pattern::Short1, config);
  assert(pattern.total == 150 && feedback_config::motorOn(pattern, 149));
  assert(!feedback_config::motorOn(pattern, 150));
  pattern = feedback_config::schedule(Pattern::Long1, config);
  assert(pattern.total == 300 && feedback_config::motorOn(pattern, 299));
  assert(!feedback_config::motorOn(pattern, 300));
  pattern = feedback_config::schedule(Pattern::Short2, config);
  assert(pattern.total == 400 && feedback_config::motorOn(pattern, 149));
  assert(!feedback_config::motorOn(pattern, 150) && !feedback_config::motorOn(pattern, 249));
  assert(feedback_config::motorOn(pattern, 250) && feedback_config::motorOn(pattern, 399));
  assert(!feedback_config::motorOn(pattern, 400));
  assert(feedback_config::schedule(Pattern::Off, config).total == 0);
  config.shortMs = 10; config.doubleGapMs = 5; config.longMs = 40;
  pattern = feedback_config::schedule(Pattern::Short2, config);
  assert(pattern.total == 25 && !feedback_config::motorOn(pattern, 10));
  assert(feedback_config::motorOn(pattern, 15) && !feedback_config::motorOn(pattern, 25));
  config.doubleGapMs = 0;
  assert(feedback_config::schedule(Pattern::Short2, config).total == 20);
  config.shortMs = 0;
  assert(feedback_config::schedule(Pattern::Short1, config).total == 0);
  assert(feedback_config::schedule(Pattern::Short2, config).total == 0);
  config.longMs = 0;
  assert(feedback_config::schedule(Pattern::Long1, config).total == 0);
  config.shortMs = UINT32_MAX; config.longMs = UINT32_MAX;
  assert(feedback_config::schedule(Pattern::Short2, config).total == 0);
  assert(feedback_config::schedule(Pattern::Long1, config).total == 0);

  config = Settings{};
  Feedback f = state(Role::Tagger, DeviceState::Blink, Phase::Ready, Display::TaggerBlink);
  assert(feedback_config::forState(f, config) == config.onTaggerBlink);
  f.deviceState = DeviceState::Activate;
  assert(feedback_config::forState(f, config) == config.onTaggerActive);
  f.deviceState = DeviceState::Setting;
  assert(feedback_config::forState(f, config) == config.onSetting);
  f.deviceState = DeviceState::Ready;
  assert(feedback_config::forState(f, config) == config.onReady);
  f.phase = Phase::Exploration;
  assert(feedback_config::forState(f, config) == config.onExploration);
  f.phase = Phase::Ended;
  assert(feedback_config::forState(f, config) == config.onEnded);
  f = state(Role::Ghost, DeviceState::Other, Phase::Active, Display::Ghost);
  assert(feedback_config::forState(f, config) == config.onGhost);
  f.role = Role::Player;
  assert(feedback_config::forState(f, config) == config.onPlayer);
  f.role = Role::Neutral;
  assert(feedback_config::forState(f, config) == Pattern::Off);
}

static void semanticTransitionsAndNoReplay() {
  FeedbackEngine engine;
  auto f = state(Role::Neutral, DeviceState::Setting, Phase::Setting, Display::Setting);
  assert(!engine.update(f, 0, false, 0).motor);  // First sync is a baseline.
  f.deviceState = DeviceState::Ready; f.phase = Phase::Ready; f.display = Display::Ready;
  assert(engine.update(f, 0, false, 1000).motor);
  assert(engine.update(f, 0, false, 1149).motor);
  assert(!engine.update(f, 0, false, 1150).motor);  // Repeated polls do not restart.
  f.lit = 3;
  assert(!engine.update(f, 0, false, 2000).motor);  // Chip/count display changes are not states.
  f.role = Role::Tagger; f.deviceState = DeviceState::Blink; f.display = Display::TaggerBlink;
  assert(engine.update(f, 0, false, 3000).motor);
  assert(!engine.update(f, 0, false, 3150).motor);
  assert(engine.update(f, 0, false, 3250).motor);
  assert(!engine.update(f, 0, false, 3400).motor);
  f.deviceState = DeviceState::Activate; f.display = Display::TaggerActive;
  assert(engine.update(f, 0, false, 4000).motor);
  assert(engine.update(f, 0, false, 4299).motor);
  assert(!engine.update(f, 0, false, 4300).motor);
  f = state(Role::Ghost, DeviceState::Other, Phase::Active, Display::Ghost);
  assert(engine.update(f, 0, false, 5000).motor);
  f.role = Role::Player; f.display = Display::Player;  // Role color is independent of the chip.
  assert(engine.update(f, 0, false, 6000).motor);
  assert(!engine.update(f, 0, false, 6150).motor);
  f.display = Display::Player; f.lit = 4;
  assert(!engine.update(f, 0, false, 6200).motor);
  f.stateEpoch++;
  f.role = Role::Ghost; f.display = Display::Ghost;
  assert(!engine.update(f, 0, false, 6300).motor);  // Reconnect/identity replacement.
  f.stateValid = false;
  assert(!engine.update(f, 3, true, 6400).motor);
  f.stateValid = true; f.role = Role::Player; f.display = Display::Player;
  assert(!engine.update(f, 0, false, 6500).motor);
  f.phase = Phase::Ended; f.display = Display::Ended;
  assert(engine.update(f, 0, false, 7000).motor);
  assert(engine.update(f, 0, false, 7299).motor);
  assert(!engine.update(f, 0, false, 7300).motor);
}

static void prioritiesAndCancellation() {
  Settings config;
  config.onPlayer = Pattern::Short2;
  FeedbackEngine engine(config);
  auto f = state(Role::Ghost, DeviceState::Other, Phase::Active, Display::Ghost);
  engine.update(f, 0, false, 0);
  f.role = Role::Player; f.display = Display::Player;
  assert(engine.update(f, 3, true, 1000).motor);
  assert(!engine.update(f, 3, true, 1200).motor);  // State OFF gap beats proximity ON.
  f.haptic = Haptic::Found;
  assert(engine.update(f, 3, true, 1250).motor);  // Explicit event replaces state pattern.
  f.haptic = Haptic::None;
  f.role = Role::Ghost; f.display = Display::Ghost;
  assert(!engine.update(f, 0, false, 1400).motor);  // Role change cannot interrupt event gap.
  assert(engine.update(f, 0, false, 1500).motor);
  assert(!engine.update(f, 0, false, 1650).motor);
  assert(!engine.update(f, 0, false, 1800).motor);  // No queued transition replay.
  f.haptic = Haptic::Removed;
  assert(engine.update(f, 0, false, 2000).motor);
  f.haptic = Haptic::None;
  f.phase = Phase::Ready; f.display = Display::Ready;
  assert(engine.update(f, 0, false, 2001).motor);  // Ready cancels old event, starts its own short pulse.
  assert(!engine.update(f, 0, false, 2151).motor);
  f.phase = Phase::Active; f.display = Display::Ghost;
  assert(engine.update(f, 0, false, 3000).motor);
  f.stateValid = false;
  assert(!engine.update(f, 0, false, 3001).motor);
  f.stateValid = true;
  assert(!engine.update(f, 0, false, 3002).motor);
  f.haptic = Haptic::Found;
  assert(!engine.update(f, 0, false, 4000, true).motor);  // OTA/reset consumes and cancels.
  f.haptic = Haptic::None; f.role = Role::Player; f.display = Display::Player;
  assert(!engine.update(f, 0, false, 4001, true).motor);
  assert(!engine.update(f, 0, false, 4002).motor);
}

static void onlyServerRoleTransitionsSignal() {
  GameModel game(Profile::Origin);
  FeedbackEngine engine;
  game.begin(true, 0);
  ServerSnapshot s;
  s.valid = true; s.phase = Phase::Active; s.deviceState = DeviceState::Activate;
  s.role = Role::Player; s.stepSeconds = 3; s.capturesAllowed = true;
  strcpy(s.session, "run1"); strcpy(s.deviceName, "G1P1");
  game.applyServer(s, 0);
  assert(!engine.update(game.feedback(), 0, false, 0).motor);
  game.chipChanged(false, 10);
  auto out = engine.update(game.feedback(), 0, false, 10);
  assert(out.green == 64 && out.lit == 4 && !out.motor);
  game.chipChanged(true, 20);
  assert(!engine.update(game.feedback(), 0, false, 20).motor);
  s.role = Role::Ghost; s.revivalCount = 2; game.applyServer(s, 1000);
  out = engine.update(game.feedback(), 0, false, 1000);
  assert(out.blue == 64 && out.lit == 2 && out.motor);
  game.applyServer(s, 1100);
  assert(engine.update(game.feedback(), 0, false, 1100).motor);
  assert(!engine.update(game.feedback(), 0, false, 1300).motor);
  game.chipChanged(false, 1400);
  assert(!engine.update(game.feedback(), 0, false, 1400).motor);
  s.role = Role::Player; game.applyServer(s, 2000);
  out = engine.update(game.feedback(), 0, false, 2000);
  assert(out.green == 64 && out.lit == 4 && out.motor);
  assert(!engine.update(game.feedback(), 0, false, 2150).motor);
}

static void wrapAndTaggerLeds() {
  FeedbackEngine engine;
  auto f = state(Role::Tagger, DeviceState::Blink, Phase::Ready, Display::TaggerBlink);
  const uint32_t start = UINT32_MAX - 200U;
  auto out = engine.update(f, 0, false, start);
  assert(out.red == 48 && out.blue == 64 && out.lit == 4 && !out.motor);
  out = engine.update(f, 0, false, 298);
  assert(out.red == 48 && out.blue == 64);  // 499 ms across wrap.
  out = engine.update(f, 0, false, 299);
  assert(out.red == 0 && out.blue == 0);
  out = engine.update(f, 0, false, 799);
  assert(out.red == 48 && out.blue == 64);
  f.deviceState = DeviceState::Activate; f.display = Display::TaggerActive;
  out = engine.update(f, 0, false, 800);
  assert(out.red == 48 && out.blue == 64 && out.motor);
  out = engine.update(f, 0, false, 1500);
  assert(out.red == 48 && out.blue == 64 && !out.motor);  // Activate never blinks.
  f.display = Display::Tagger;
  out = engine.update(f, 0, false, 1700);
  assert(out.red == 48 && out.blue == 64);

  FeedbackEngine wrap;
  f = state(Role::Player, DeviceState::Other, Phase::Active, Display::Player);
  wrap.update(f, 0, false, start - 1);
  f.role = Role::Ghost; f.display = Display::Ghost;
  assert(wrap.update(f, 0, false, start).motor);
  assert(wrap.update(f, 0, false, 98).motor);
  assert(!wrap.update(f, 0, false, 99).motor);  // 300 ms transition pulse across wrap.
}

static void commandSchedules() {
  Settings config;
  auto s = feedback_config::pulses(200, 200, 3);
  assert(s.total == 1000 && s.count == 3 && s.onMs == 200 && s.gapMs == 200);
  assert(feedback_config::motorOn(s, 0) && feedback_config::motorOn(s, 199));
  assert(!feedback_config::motorOn(s, 200) && !feedback_config::motorOn(s, 399));
  assert(feedback_config::motorOn(s, 400) && feedback_config::motorOn(s, 599));
  assert(!feedback_config::motorOn(s, 600) && !feedback_config::motorOn(s, 799));
  assert(feedback_config::motorOn(s, 800) && feedback_config::motorOn(s, 999));
  assert(!feedback_config::motorOn(s, 1000));
  assert(feedback_config::pulses(0, 200, 3).total == 0);       // Zero ON disables.
  assert(feedback_config::pulses(200, 200, 0).total == 0);     // Zero count disables.
  assert(feedback_config::pulses(UINT32_MAX, 0, 2).total == 0);  // Overflow rejected.
  assert(feedback_config::pulses(300, 0, 1).total == 300);

  assert(feedback_config::commandSchedule(12, config).total == 200);
  assert(feedback_config::commandSchedule(13, config).total == 600);
  assert(feedback_config::commandSchedule(14, config).total == 1000);
  assert(feedback_config::commandSchedule(15, config).total == 600);
  assert(feedback_config::commandSchedule(16, config).total == 1400);
  assert(feedback_config::commandSchedule(17, config).total == 2200);
  assert(feedback_config::commandSchedule(0, config).total == 0);
  assert(feedback_config::commandSchedule(11, config).total == 0);
  assert(feedback_config::commandSchedule(18, config).total == 0);
  assert(feedback_config::isCommand(12) && feedback_config::isCommand(17));
  assert(!feedback_config::isCommand(11) && !feedback_config::isCommand(18));
  assert(feedback_config::kVibeMute == 10 && feedback_config::kVibeOn == 11);

  // Existing state patterns keep their exact shape on the generalized schedule.
  s = feedback_config::schedule(Pattern::Short2, config);
  assert(s.onMs == 150 && s.gapMs == 100 && s.count == 2 && s.total == 400);
  s = feedback_config::schedule(Pattern::Long1, config);
  assert(s.onMs == 300 && s.gapMs == 0 && s.count == 1 && s.total == 300);
}

int main() {
  patternsAndConfiguration();
  semanticTransitionsAndNoReplay();
  prioritiesAndCancellation();
  onlyServerRoleTransitionsSignal();
  wrapAndTaggerLeds();
  commandSchedules();
  puts("PASS: configured state haptics, priorities, reconnect suppression, authoritative roles, tagger LEDs and operator vibe commands");
}
