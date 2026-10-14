#include "../pixel_refresh.h"
#include "../feedback.h"

#include <assert.h>
#include <stdio.h>

using namespace iotglove;

static void continuousMotorAndIdle() {
  PixelRefresh refresh;
  assert(refresh.update(false, true, 0));  // Initial LED frame.
  assert(!refresh.update(false, false, 1000));
  assert(!refresh.update(true, false, 1100));  // No extra write at motor start.
  assert(!refresh.update(true, false, 1599));
  assert(refresh.update(true, false, 1600));
  assert(!refresh.update(true, false, 2099));
  assert(refresh.update(true, false, 2100));
  assert(!refresh.update(false, false, 2110));
  assert(!refresh.update(false, false, 2159));
  assert(refresh.update(false, false, 2160));
  assert(!refresh.update(false, false, 10000));  // Exactly one post-OFF resend.
}

static void shortDoublePulseAndRestart() {
  PixelRefresh refresh;
  assert(refresh.update(true, true, 1000));
  assert(!refresh.update(false, false, 1150));
  assert(refresh.update(false, false, 1200));  // 100ms gap between short pulses.
  assert(!refresh.update(true, false, 1250));
  assert(!refresh.update(false, false, 1400));
  assert(refresh.update(false, false, 1450));
  assert(!refresh.update(false, false, 1500));

  assert(!refresh.update(true, false, 2000));
  assert(!refresh.update(false, false, 2100));  // Cancellation is an ordinary OFF edge.
  assert(!refresh.update(true, false, 2140));  // Restart cancels old deferred resend.
  assert(!refresh.update(true, false, 2150));
  assert(!refresh.update(true, false, 2639));
  assert(refresh.update(true, false, 2640));
  assert(!refresh.update(false, false, 2700));
  assert(refresh.update(false, false, 2800));  // A late loop performs one resend.
  assert(!refresh.update(false, false, 2801));
}

static void visualChangesStayImmediate() {
  PixelRefresh refresh;
  assert(refresh.update(false, true, 0));
  assert(!refresh.update(true, false, 1000));
  assert(refresh.update(true, true, 1499));
  assert(!refresh.update(true, false, 1500));  // Visual write already refreshed LEDs.
  assert(!refresh.update(true, false, 1998));
  assert(refresh.update(true, false, 1999));
  assert(!refresh.update(false, false, 2000));
  assert(refresh.update(false, true, 2010));  // Never delay a changed frame.
  assert(!refresh.update(false, false, 2049));
  assert(refresh.update(false, false, 2050));  // Still recover after the settling interval.
  assert(!refresh.update(false, false, 2051));
  assert(!refresh.update(true, false, 2100));
  assert(!refresh.update(false, false, 2200));
  assert(refresh.update(false, true, 2250));  // Coincident requests use a single write.
  assert(!refresh.update(false, false, 2251));
}

static void millisWrap() {
  PixelRefresh refresh;
  const uint32_t start = UINT32_MAX - 200U;
  assert(refresh.update(true, true, start));
  assert(!refresh.update(true, false, uint32_t(start + 499U)));
  assert(refresh.update(true, false, uint32_t(start + 500U)));

  PixelRefresh offRefresh;
  assert(offRefresh.update(true, true, UINT32_MAX - 100U));
  assert(!offRefresh.update(false, false, UINT32_MAX - 20U));
  assert(!offRefresh.update(false, false, 28U));
  assert(offRefresh.update(false, false, 29U));
  assert(!offRefresh.update(false, false, 30U));
}

static void currentDarkFrameIsPreserved() {
  FeedbackEngine engine;
  PixelRefresh refresh;
  Feedback state;
  state.stateValid = true;
  state.role = Role::Tagger;
  state.deviceState = DeviceState::Blink;
  state.phase = Phase::Active;
  state.display = Display::TaggerBlink;
  state.lit = 4;
  Outputs out = engine.update(state, 11, false, 0);
  assert(out.motor && out.red != 0 && refresh.update(out.motor, true, 0));
  out = engine.update(state, 11, false, 500);
  assert(out.motor && out.red == 0 && out.green == 0 && out.blue == 0);
  assert(refresh.update(out.motor, true, 500));  // Blink OFF stays black during vibration.
  out = engine.update(state, 10, false, 550);
  assert(!out.motor && !refresh.update(out.motor, false, 550));
  out = engine.update(state, 10, false, 600);
  assert(refresh.update(out.motor, false, 600));
  assert(out.red == 0 && out.green == 0 && out.blue == 0);

  state.role = Role::Ghost;
  state.deviceState = DeviceState::Other;
  state.display = Display::Ghost;
  state.lit = 0;
  out = engine.update(state, 11, false, 1000);
  assert(out.motor && out.lit == 0 && refresh.update(out.motor, true, 1000));
  out = engine.update(state, 11, false, 1500);
  assert(refresh.update(out.motor, false, 1500) && out.lit == 0);
}

int main() {
  continuousMotorAndIdle();
  shortDoublePulseAndRestart();
  visualChangesStayImmediate();
  millisWrap();
  currentDarkFrameIsPreserved();
  puts("pixel refresh tests passed");
}
