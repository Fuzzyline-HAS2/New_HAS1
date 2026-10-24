#include "../button_trace.h"
#include "../feedback.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
using namespace iotglove;
static ServerSnapshot server(Role role = Role::Ghost) {
  ServerSnapshot s;
  s.valid = true; s.phase = Phase::Active; s.role = role;
  s.deviceState = DeviceState::Activate; s.capturesAllowed = true;
  s.stepSeconds = 3;
  strcpy(s.session, "game:1"); strcpy(s.deviceName, "G1P1");
  return s;
}
static void decisionsAndIdentity() {
  GameModel game(Profile::Origin); game.begin(true, 0);
  assert(game.buttonPressed(1, 11, true).disposition == ButtonDisposition::Unsynchronized);
  game.applyServer(server(Role::Player), 2);
  assert(game.buttonPressed(3, 12, true).disposition == ButtonDisposition::NotGhost);
  game.applyServer(server(Role::Tagger), 4);
  assert(game.buttonPressed(5, 13, true).disposition == ButtonDisposition::NotGhost);
  assert(game.queuedEvents() == 0 && game.feedback().haptic == Haptic::None);
  game.applyServer(server(), 6);
  auto d = game.buttonPressed(7, 14, true);
  assert(d.disposition == ButtonDisposition::Queued && d.sequence != 0);
  GameEvent e; assert(game.peekEvent(e));
  assert(e.sequence == d.sequence && e.pressId == 14 && e.traceButton && e.value == 0);
  const auto f = game.feedback();
  assert(f.haptic == Haptic::Found && f.hapticPressId == 14);
  assert(game.feedback().hapticPressId == 0);
  game.consumeEvent();
  game.tick(3007); assert(game.peekEvent(e));
  assert(e.value == 1 && e.pressId == 0 && !e.traceButton); // Timer is not a button.
  game.consumeEvent();
  for (unsigned i = 0; i < 12; ++i)
    assert(game.buttonPressed(4000+i, 20+i, false).disposition == ButtonDisposition::Queued);
  assert(game.buttonPressed(5000, 99, true).disposition == ButtonDisposition::QueueFull);
  assert(game.queueOverflowed() && !game.synchronized() && game.queuedEvents() == 12);
  assert(game.buttonPressed(5001, 100, true).disposition == ButtonDisposition::Unsynchronized);
  GameModel training(Profile::Training); training.begin(false, 0);
  assert(training.buttonPressed(20, 101, true).disposition == ButtonDisposition::LocalAccepted);
  assert(training.queuedEvents() == 0 && training.feedback().hapticPressId == 101);
  GameModel academy(Profile::Origin); academy.begin(false, 0);
  auto a = server(Role::Neutral); a.phase = Phase::Academy; a.deviceState = DeviceState::Player;
  academy.applyServer(a, 1);
  assert(academy.buttonPressed(20, 102, true).disposition == ButtonDisposition::LocalAccepted);
  assert(academy.queuedEvents() == 0 && academy.feedback().hapticPressId == 102);
}
static void boundedInputsAndDebounce() {
  DebouncedInput input; input.begin(false, 0);
  assert(!input.update(true, 1)); assert(!input.update(false, 10));
  assert(!input.update(true, 20)); assert(!input.update(true, 49));
  assert(input.update(true, 50)); assert(!input.update(true, 999));
  assert(!input.update(false, 1000)); assert(input.update(false, 1030));
  ButtonTrace trace;
  for (unsigned i = 1; i <= 7; ++i) {
    assert(trace.press(i) == i);
    assert(trace.selected() == (i <= 4));
    assert(trace.suppressed(i) == 0);
  }
  assert(trace.suppressed(10001) == 3); // Summary appears even if chatter stops.
  assert(trace.suppressed(10002) == 0);
  assert(trace.press(10003) == 8 && trace.selected());
  TraceBudget wrap;
  assert(wrap.take(UINT32_MAX-10,1,100));
  assert(!wrap.take(10,1,100)); assert(wrap.take(100,1,100));
  assert(wrap.takeSuppressed(100,100) == 1);
}
static void realMotorEdgesAndSources() {
  GameModel game(Profile::Origin); game.begin(true, 0); game.applyServer(server(), 1);
  FeedbackEngine engine; engine.update(game.feedback(),0,false,1);
  MotorTrace trace; assert(trace.observe(1,false));
  game.buttonPressed(100, 77, true);
  auto out = engine.update(game.feedback(),0,false,100);
  assert(out.motor && out.motorSource == MotorSource::Event && out.motorPressId == 77);
  assert(trace.observe(100,out.motor));
  for (uint32_t t = 101; t < 250; ++t) {
    out=engine.update(game.feedback(),0,false,t); assert(!trace.observe(t,out.motor));
  }
  out=engine.update(game.feedback(),0,false,250);
  assert(!out.motor && out.motorPressId == 77 && trace.observe(250,out.motor));
  out=engine.update(game.feedback(),0,false,350);
  assert(out.motor && out.motorPressId == 77 && trace.observe(350,out.motor));
  out=engine.update(game.feedback(),0,false,500);
  assert(!out.motor && out.motorPressId == 0 && trace.observe(500,out.motor));
  // No 500 ms output gate can swallow the 150/100/150 pulse edges.
  game.buttonPressed(600,78,true);
  out=engine.update(game.feedback(),11,true,600);
  assert(out.motor && out.motorSource == MotorSource::Forced && out.motorPressId == 0);
  game.buttonPressed(700,79,true);
  out=engine.update(game.feedback(),10,true,700);
  assert(!out.motor && out.motorPressId == 0); // Muted button does not invent a motor event.
  out=engine.update(game.feedback(),3,true,1000);
  assert(out.motor && out.motorSource == MotorSource::Proximity && out.motorPressId == 0);
  MotorTrace noise;
  unsigned accepted=0;
  for (unsigned i=0;i<50;++i) accepted += noise.observe(i, (i%2)==0);
  assert(accepted==16 && noise.suppressed(1000)==34);
  assert(noise.suppressed(1001)==0);
}
static void observationalParity() {
  // Replay the same gameplay with tracing enabled/disabled: metadata must not
  // affect state, admission, timer progress, haptic waveforms or queue order.
  GameModel plain(Profile::Origin), traced(Profile::Origin);
  FeedbackEngine a,b;
  plain.begin(true,0); traced.begin(true,0);
  plain.applyServer(server(),1); traced.applyServer(server(),1);
  for (uint32_t t=1; t<20000; ++t) {
    if (t%731==0) { plain.buttonPressed(t); traced.buttonPressed(t,t,true); }
    plain.tick(t); traced.tick(t);
    auto x=a.update(plain.feedback(),0,false,t), y=b.update(traced.feedback(),0,false,t);
    assert(x.motor==y.motor && x.red==y.red && x.green==y.green && x.blue==y.blue && x.lit==y.lit);
    assert(plain.count()==traced.count() && plain.synchronized()==traced.synchronized());
    if (t%1000==0) {
      GameEvent p,q; bool have=plain.peekEvent(p); assert(have==traced.peekEvent(q));
      if (have) { assert(p.sequence==q.sequence && p.value==q.value); plain.consumeEvent(); traced.consumeEvent(); }
    }
  }
}
int main() {
  decisionsAndIdentity(); boundedInputsAndDebounce(); realMotorEdgesAndSources(); observationalParity();
  puts("PASS: button decisions, causal IDs, debounce, bounded chatter, full queue, complete motor pulses and gameplay parity");
}
