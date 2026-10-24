#include "network_restart.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
using namespace iotglove;
static NetworkDiagnostics offline(uint32_t now, bool wasUp=true, uint32_t since=0) {
  NetworkDiagnostics d;
  d.phase=NetworkPhase::Backoff; d.sequence=now; d.startedAtMs=now; d.progressAtMs=now;
  d.deadlineMs=20000; d.wifiWasUp=wasUp; d.wifiDownSinceMs=since;
  return d;
}
static NetworkDiagnostics online(uint32_t now) {
  auto d=offline(now); d.wifiConnected=true; return d;
}
static void retainedUnlimitedAndSaturation() {
  NetworkRestartHistory h{}; NetworkRestartPolicy::boot(h,false,false);
  assert(h.magic==NetworkRestartPolicy::kMagic && h.attempts==0);
  for (unsigned expected=1;expected<=300;++expected) {
    NetworkRestartPolicy policy;
    const uint32_t delay=NetworkRestartPolicy::backoffMs(h.attempts);
    auto r=policy.poll(h,offline(30000),30000,0);
    if (expected==1) assert(r.restart);
    else {
      assert(!r.restart && r.waitingStarted && r.waitMs==delay);
      assert(!policy.poll(h,offline(delay-1),delay-1,0).restart);
      r=policy.poll(h,offline(delay),delay,0); assert(r.restart);
    }
    assert(h.attempts==expected && h.requested);
    NetworkRestartPolicy::boot(h,true,false);
    assert(h.attempts==expected && !h.requested);
  }
  assert(NetworkRestartPolicy::diagnosticAttempt(254)==254);
  assert(NetworkRestartPolicy::diagnosticAttempt(255)==255);
  assert(NetworkRestartPolicy::diagnosticAttempt(256)==255);
  assert(NetworkRestartPolicy::diagnosticAttempt(UINT32_MAX)==255);
  h.attempts=UINT32_MAX;
  NetworkRestartPolicy saturated;
  assert(saturated.poll(h,offline(1800000),1800000,0).restart);
  assert(h.attempts==UINT32_MAX && h.requested);
  NetworkRestartPolicy::boot(h,true,false); assert(h.attempts==UINT32_MAX);
  NetworkRestartPolicy::boot(h,false,true); assert(h.attempts==UINT32_MAX);
  h={NetworkRestartPolicy::kMagic,256,false}; NetworkRestartPolicy::boot(h,false,true);
  assert(h.attempts==257); // Actual watchdog boots count beyond 3/255 too.
  NetworkRestartPolicy::boot(h,false,false); assert(h.attempts==0); // Cold boot.
  h={0x47575232U,3,true}; NetworkRestartPolicy::boot(h,true,false);
  assert(h.magic==NetworkRestartPolicy::kMagic && h.attempts==0); // Old layout policy invalidated.
}
static void thresholdsAndPersistentStall() {
  NetworkRestartHistory h{NetworkRestartPolicy::kMagic,0,false};
  NetworkRestartPolicy policy;
  assert(!policy.poll(h,offline(29999),29999,5000).restart);
  assert(policy.poll(h,offline(30000),30000,5000).restart); // First keeps exact old threshold.
  h={NetworkRestartPolicy::kMagic,0,false}; NetworkRestartPolicy never;
  assert(!never.poll(h,offline(179999,false),179999,0).restart);
  assert(never.poll(h,offline(180000,false),180000,0).restart);
  h={NetworkRestartPolicy::kMagic,1,false}; NetworkRestartPolicy stall;
  auto d=online(0);d.phase=NetworkPhase::Http;
  assert(!stall.poll(h,d,19999,5000).restart);
  auto r=stall.poll(h,d,20000,5000);
  assert(r.waitingStarted && !r.restart && r.waitMs==305000 && r.remainingMs==285000);
  // A one-shot log at 20s must not suppress the later decision: production poll
  // is called each loop even though no new waitingStarted/stall report is due.
  for (uint32_t now=21000;now<305000;now+=1000) {
    r=stall.poll(h,d,now,0); assert(!r.waitingStarted && !r.restart);
  }
  assert(stall.poll(h,d,305000,0).restart); // Entropy captured once, not jittered every pass.
}
static void recoveryOtaAndChangingCause() {
  NetworkRestartHistory h{NetworkRestartPolicy::kMagic,1,false}; NetworkRestartPolicy policy;
  assert(policy.poll(h,offline(30000),30000,0).waitingStarted);
  auto r=policy.poll(h,online(50000),50000,0);
  assert(r.cancelled && r.cause==3 && r.elapsedMs==50000 && h.attempts==1);
  assert(!policy.poll(h,online(300000),300000,0).restart);
  assert(policy.poll(h,offline(330000,true,300000),330000,0).waitingStarted);
  r=policy.poll(h,offline(350000,true,300000),350000,0,true);
  assert(r.cancelled && !r.restart); // Pair-level OTA guard, not just worker phase.
  auto ota=offline(2000000);ota.phase=NetworkPhase::Ota;
  assert(!policy.poll(h,ota,2000000,0).restart);
  NetworkRestartPolicy changes;
  auto d=offline(30000);assert(changes.poll(h,d,30000,0).waitingStarted);
  d=offline(50000);d.phase=NetworkPhase::Http;d.startedAtMs=30000;
  r=changes.poll(h,d,50000,0);assert(r.cause==2 && !r.waitingStarted && r.elapsedMs==50000);
  r=changes.poll(h,offline(100000),100000,0);assert(r.cause==3 && !r.waitingStarted);
  assert(changes.poll(h,offline(300000),300000,0).restart); // Cause swaps never push deadline out.
}
static void stableMeansConnectedAndProgressing() {
  NetworkRestartHistory h{NetworkRestartPolicy::kMagic,99,false}; NetworkRestartPolicy policy;
  // Reconnect attempts that keep advancing while disconnected cannot restore.
  for (uint32_t now=0;now<=600000;now+=1000) {
    auto r=policy.poll(h,offline(now,true,now),now,0);assert(!r.restored);
  }
  assert(h.attempts==99);
  // Ten connected minutes with real progress restores the backoff.
  for (uint32_t now=601000;now<1201000;now+=1000)
    assert(!policy.poll(h,online(now),now,0).restored);
  auto r=policy.poll(h,online(1201000),1201000,0);
  assert(r.restored && r.previousAttempts==99 && h.attempts==0);
  h.attempts=5;NetworkRestartPolicy frozen;
  auto d=online(0);d.deadlineMs=0; // Even stale diagnostics without a deadline cannot earn reset.
  for(uint32_t now=0;now<1800000;now+=1000) assert(!frozen.poll(h,d,now,0).restored);
  assert(h.attempts==5);
  NetworkRestartPolicy interrupted;
  for(uint32_t now=0;now<400000;now+=1000) interrupted.poll(h,online(now),now,0);
  interrupted.poll(h,offline(400000,true,400000),400000,0);
  assert(!interrupted.poll(h,online(601000),601000,0).restored);
}
static void rollover() {
  const uint32_t start=UINT32_MAX-10000U;
  NetworkRestartHistory h{NetworkRestartPolicy::kMagic,1,false};NetworkRestartPolicy p;
  assert(p.poll(h,offline(start+30000U,true,start),start+30000U,0).waitingStarted);
  assert(!p.poll(h,offline(start+299999U,true,start),start+299999U,0).restart);
  assert(p.poll(h,offline(start+300000U,true,start),start+300000U,0).restart);
}
int main() {
  retainedUnlimitedAndSaturation(); thresholdsAndPersistentStall();
  recoveryOtaAndChangingCause(); stableMeansConnectedAndProgressing(); rollover();
  puts("PASS: unlimited paced restart, >255/saturation, persistent fault due, recovery/OTA cancellation, cause swaps, true link health and wrap");
}
