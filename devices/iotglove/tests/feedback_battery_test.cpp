#include "../feedback.h"
#include "../battery.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

using namespace iotglove;

int main() {
  FeedbackEngine engine;
  Feedback state;
  state.display = Display::Ghost;
  state.lit = 2;
  state.haptic = Haptic::Removed;
  auto out = engine.update(state, 3, true, UINT32_MAX - 100U);
  assert(out.motor && out.blue == 64 && out.lit == 2);
  state.haptic = Haptic::None;
  assert(engine.update(state, 3, true, 198).motor);
  assert(!engine.update(state, 3, true, 199).motor);
  state.haptic = Haptic::Found;
  assert(engine.update(state, 0, true, 1000).motor);
  state.haptic = Haptic::None;
  assert(engine.update(state, 0, true, 1149).motor);
  assert(!engine.update(state, 0, true, 1150).motor);
  assert(!engine.update(state, 0, true, 1249).motor);
  assert(engine.update(state, 0, true, 1250).motor);
  assert(!engine.update(state, 0, true, 1400).motor);
  state.display = Display::Player;
  assert(engine.update(state, 3, true, 2000).motor);
  assert(!engine.update(state, 3, false, 2000).motor);
  assert(!engine.update(state, 3, true, 2150).motor);
  assert(engine.update(state, 3, true, 2250).motor);
  assert(!engine.update(state, 1, true, 2250).motor);
  state.display = Display::Ended;
  assert(!engine.update(state, 3, true, 4000).motor);
  state.display = Display::Ghost; state.haptic = Haptic::Removed;
  assert(engine.update(state, 0, false, 5000).motor);
  state.display = Display::Ended; state.haptic = Haptic::None;
  assert(!engine.update(state, 0, false, 5001).motor);  // stop cancels active pattern

  float voltage = -1;
  BatterySampler disabled(0, 1, 0, 0);
  assert(!disabled.configured() && !disabled.add(2000, voltage));
  BatterySampler battery(2, 1, 3000, 4300);
  assert(battery.configured());
  for (unsigned n = 0; n < 15; ++n) assert(!battery.add(2000, voltage));
  assert(battery.add(2000, voltage));
  assert(fabsf(voltage - 4.0f) < 0.001f);
  voltage = -1;
  for (unsigned n = 0; n < 16; ++n) assert(!battery.add(n == 7 ? 0 : 2000, voltage));
  assert(voltage == -1);
  for (unsigned n = 0; n < 16; ++n) assert(!battery.add(3101, voltage));
  for (unsigned n = 0; n < 16; ++n) assert(!battery.add(2200, voltage));
  for (unsigned n = 0; n < 15; ++n) assert(!battery.add(1900, voltage));
  assert(battery.add(1900, voltage) && fabsf(voltage - 3.8f) < 0.001f);
  puts("PASS: production nonblocking haptics/LED and calibrated battery batching");
}
