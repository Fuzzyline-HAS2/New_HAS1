#include "network_recovery.h"
#include <assert.h>
#include <stdio.h>
#include <initializer_list>
using namespace iotglove;

static void threeFailures(NetworkRecoveryPolicy& p, uint32_t start) {
  p.observePrimary(start, true, false);
  p.observePrimary(start + 1000, true, false);
  p.observePrimary(start + 15000, true, false);
}

int main() {
  NetworkRecoveryPolicy p;
  p.observePrimary(0, true, false);
  p.observePrimary(1, true, false);
  assert(!p.shouldReconnect(15000)); // The elapsed threshold alone is insufficient.
  p.observePrimary(2, true, false);
  assert(!p.shouldReconnect(14999));
  assert(p.shouldReconnect(15000));
  p.reconnected(15000, 5000);
  assert(p.reconnects() == 1 && p.cooldownMs() == 35000);
  threeFailures(p, 16000);
  assert(!p.shouldReconnect(49999));
  assert(p.shouldReconnect(50000));
  p.reconnected(50000, 5001); // Jitter stays within 0..5000.
  assert(p.cooldownMs() == 60000);
  threeFailures(p, 51000);
  assert(!p.shouldReconnect(109999));
  assert(p.shouldReconnect(110000));
  p.reconnected(110000, 1);
  assert(p.cooldownMs() == 120001);
  p.observePrimary(110001, false, true); // One good sample must not reset escalation.
  threeFailures(p, 110002);
  assert(!p.shouldReconnect(230000));
  assert(p.shouldReconnect(230001));
  p.reconnected(230001, 5000);
  assert(p.cooldownMs() == 125000);
  p.observePrimary(240000, false, true);
  p.observePrimary(299999, false, true);
  assert(p.cooldownMs() == 125000);
  p.observePrimary(300000, false, true);
  assert(p.cooldownMs() == 0);
  threeFailures(p, 300001);
  assert(p.shouldReconnect(315001));
  p.reconnected(315001, 0);
  assert(p.cooldownMs() == 30000);

  // Responding server errors and invalid JSON break transport streaks; neither
  // represents valid synchronization or the sustained-health reset condition.
  for (int response : {429, 500, 503, 0}) {
    (void)response;
    NetworkRecoveryPolicy errors;
    for (uint32_t now = 0; now <= 600000; now += 1000) {
      errors.observePrimary(now, false, false);
      assert(!errors.shouldReconnect(now));
    }
    threeFailures(errors, 700000);
    errors.observePrimary(715001, false, false);
    assert(errors.failures() == 0 && !errors.shouldReconnect(900000));
  }
  NetworkRecoveryPolicy wrap;
  const uint32_t start = UINT32_MAX - 9999;
  threeFailures(wrap, start);
  assert(!wrap.shouldReconnect(start + 14999));
  assert(wrap.shouldReconnect(start + 15000));
  wrap.reconnected(start + 15000, 0);
  threeFailures(wrap, start + 16000);
  assert(!wrap.shouldReconnect(start + 44999));
  assert(wrap.shouldReconnect(start + 45000));
  wrap.disconnected();
  assert(wrap.failures() == 0); // Ordinary AP reconnect remains independent.
  assert(wrap.cooldownMs() == 30000);

  NetworkRecoveryPolicy retry;
  retry.defer(UINT32_MAX - 99, 500);
  assert(retry.deferred(399));
  assert(!retry.deferred(400));
  retry.defer(1000, UINT32_MAX);
  retry.defer(1001, 1); // A shorter concurrent response cannot erase the wait.
  assert(retry.deferred(120999));
  assert(!retry.deferred(121000));

  NetworkDiagnostics d;
  d.phase = NetworkPhase::Http; d.deadlineMs = 20000;
  d.startedAtMs = UINT32_MAX - 99;
  d.progressAtMs = 19000; // A trickle cannot extend the total deadline.
  assert(!networkWorkerStalled(d, 19899));
  assert(networkWorkerStalled(d, 19900));
  d.phase = NetworkPhase::Backoff; d.startedAtMs = 0; d.deadlineMs = 25000;
  assert(!networkWorkerStalled(d, 5000));
  assert(networkWorkerStalled(d, 25000)); // Deliberate waits are still bounded.
  d.phase = NetworkPhase::Ota; d.deadlineMs = 130000;
  d.idleDeadlineMs = 30000; d.progressAtMs = 90000;
  assert(!networkWorkerStalled(d, 119999));
  assert(!networkWorkerStalled(d, 120000));
  d.progressAtMs = 129999;
  assert(!networkWorkerStalled(d, 130000));
  assert(!networkWorkerStalled(d, UINT32_MAX));
  d.phase = NetworkPhase::Http; d.startedAtMs = 200000;
  d.deadlineMs = 20000; d.idleDeadlineMs = 0;
  assert(!networkWorkerStalled(d, 219999));
  assert(networkWorkerStalled(d, 220000));
  puts("PASS: primary transport recovery, bounded jitter/backoff, rollover and operation watchdog");
}
