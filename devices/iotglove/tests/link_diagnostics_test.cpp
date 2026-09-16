#include "../link_diagnostics.h"

#include <assert.h>
#include <stdio.h>

using namespace iotglove;

static wire::Frame helloFrame(uint32_t id, uint32_t boot, uint32_t firmware = 1, uint32_t partition = 1) {
  wire::Frame frame;
  strcpy(frame.type, "HELLO"); frame.id = id;
  wire::put(frame, "beetle");
  char value[16];
  snprintf(value, sizeof(value), "%lu", (unsigned long)firmware); wire::put(frame, value);
  snprintf(value, sizeof(value), "%lu", (unsigned long)partition); wire::put(frame, value);
  snprintf(value, sizeof(value), "%lu", (unsigned long)boot); wire::put(frame, value);
  return frame;
}

static bool hello(PeerState& peer, LinkDiagnostics& diagnostic, uint32_t id, uint32_t boot, uint32_t now) {
  assert(peer.receive(helloFrame(id, boot), now));
  return diagnostic.observeHello(id, peer, now);
}

static void heart(PeerState& peer, uint32_t now, uint32_t id = 0) {
  wire::Frame frame;
  strcpy(frame.type, "HEART"); frame.id = id;
  wire::put(frame, "12345"); wire::put(frame, "1"); wire::put(frame, "0");
  assert(!peer.receive(frame, now));  // HEART must not be routed to HELLO proof.
}

static void exactProbeAndFreshness() {
  PeerState peer;
  LinkDiagnostics diagnostic;
  assert(!peer.known() && !peer.online(100) && !peer.heartbeatKnown());
  assert(diagnostic.beginProbe(5, 100));
  assert(!diagnostic.beginProbe(6, 101));  // auto/manual probes cannot overwrite a challenge
  assert(!hello(peer, diagnostic, 0, 10, 110));
  heart(peer, 120, 5);
  assert(diagnostic.probeState() == ProbeState::Waiting);
  assert(!hello(peer, diagnostic, 4, 10, 130));
  diagnostic.tick(1600);
  assert(diagnostic.probeState() == ProbeState::Timeout);
  assert(!hello(peer, diagnostic, 5, 10, 1601));
  assert(!diagnostic.matchedFresh(peer, 1601));
  assert(diagnostic.beginProbe(6, 1700));
  assert(hello(peer, diagnostic, 6, 10, 1711));
  assert(diagnostic.probeRtt() == 11 && diagnostic.matchedFresh(peer, 1711));
  assert(!hello(peer, diagnostic, 6, 10, 2000));  // duplicate does not refresh RTT timestamp
  assert(diagnostic.probesSent() == 2 && diagnostic.probesMatched() == 1 && diagnostic.probeTimeouts() == 1);
  assert(diagnostic.matchedFresh(peer, 6710));
  assert(!diagnostic.matchedFresh(peer, 6711));
  assert(diagnostic.beginProbe(7, 6800));
  assert(!hello(peer, diagnostic, 6, 10, 6801));
  assert(!diagnostic.matchedFresh(peer, 6801));
  assert(hello(peer, diagnostic, 7, 10, 6802));
  hello(peer, diagnostic, 0, 11, 6803);
  assert(!diagnostic.matchedFresh(peer, 6803));  // proof belongs to the previous boot
}

static void resetProofBelongsToActualPostPulseProbe() {
  PeerState peer;
  LinkDiagnostics diagnostic;
  hello(peer, diagnostic, 0, 10, 0);
  assert(diagnostic.beginProbe(1, 20));
  diagnostic.beginReset(peer, 30);  // called at actual HIGH, after any cooldown/OTA wait
  assert(diagnostic.resetBaselineKnown() && diagnostic.resetBaselineBoot() == 10);
  assert(hello(peer, diagnostic, 1, 11, 40));
  assert(diagnostic.resetState() == ResetObservation::Waiting);  // PING predates pulse
  assert(!hello(peer, diagnostic, 0, 12, 50));  // unsolicited new boot is insufficient
  assert(diagnostic.beginProbe(2, 60));
  assert(hello(peer, diagnostic, 2, 10, 70));
  assert(diagnostic.resetState() == ResetObservation::Waiting);  // do not mix earlier new boot with old response
  assert(diagnostic.beginProbe(3, 80));
  assert(hello(peer, diagnostic, 3, 13, 90));
  assert(diagnostic.resetState() == ResetObservation::RebootObserved);
  assert(diagnostic.resetObservedBoot() == 13);
}

static void unknownStaleAndTimeoutReset() {
  PeerState peer;
  LinkDiagnostics diagnostic;
  diagnostic.beginReset(peer, 1);
  assert(!diagnostic.resetBaselineKnown());
  assert(diagnostic.beginProbe(1, 2));
  assert(hello(peer, diagnostic, 1, 10, 3));
  assert(diagnostic.resetState() == ResetObservation::BaselineUnknown);
  heart(peer, 10000);
  assert(peer.online(10000) && !peer.helloFresh(10000));
  diagnostic.beginReset(peer, 10000);
  assert(!diagnostic.resetBaselineKnown());  // a fresh HEART cannot freshen the boot baseline
  diagnostic.tick(25000);
  assert(diagnostic.resetState() == ResetObservation::Timeout);

  hello(peer, diagnostic, 0, 11, 26000);
  diagnostic.beginReset(peer, 26001);
  assert(diagnostic.beginProbe(2, 40001));
  assert(hello(peer, diagnostic, 2, 12, 41001));  // exact deadline: UART success, reset not confirmed
  assert(diagnostic.resetState() == ResetObservation::Timeout);
}

static void heartbeatAndRollover() {
  PeerState peer;
  LinkDiagnostics diagnostic;
  assert(peer.receive(helloFrame(0, 10, 8, 2), 100));
  assert(peer.firmwareVersion() == 8 && peer.partitionVersion() == 2 && peer.bootId() == 10);
  assert(peer.lastSeenAge(110) == 10 && peer.helloAge(110) == 10);
  heart(peer, 200);
  assert(peer.heartbeatKnown() && peer.heartbeatFresh(5199) && !peer.heartbeatFresh(5200));
  assert(peer.heartbeatUptime() == 12345 && peer.heartbeatScanEnabled() && !peer.heartbeatOtaBusy());
  assert(peer.receive(helloFrame(0, 11), 5201));
  assert(!peer.heartbeatKnown() && !peer.heartbeatFresh(5201));

  const uint32_t start = UINT32_MAX - 100U;
  hello(peer, diagnostic, 0, 20, start);
  diagnostic.beginReset(peer, start + 10);
  assert(diagnostic.beginProbe(3, start + 20));
  assert(hello(peer, diagnostic, 3, 21, 20));
  assert(diagnostic.probeRtt() == 101 && diagnostic.resetState() == ResetObservation::RebootObserved);
  assert(diagnostic.beginProbe(4, UINT32_MAX - 20U));
  diagnostic.tick(1479);
  assert(diagnostic.probeState() == ProbeState::Timeout);
}

static void boundedLogging() {
  DiagnosticLog log;
  assert(log.append("%s", "before"));
  char tooLong[1600]; memset(tooLong, 'x', sizeof(tooLong) - 1); tooLong[sizeof(tooLong) - 1] = '\0';
  assert(!log.append("%s", tooLong));
  assert(log.pending() == 6 && strcmp(log.data(), "before") == 0 && log.dropped() == 1);
  log.consumed(2);
  assert(log.append("%s", "after") && log.pending() == 9);
  assert(strcmp(log.data(), "foreafter") == 0);
  log.consumed(999);
  assert(log.pending() == 0 && log.append("next\n") && strcmp(log.data(), "next\n") == 0);
}

int main() {
  exactProbeAndFreshness();
  resetProofBelongsToActualPostPulseProbe();
  unknownStaleAndTimeoutReset();
  heartbeatAndRollover();
  boundedLogging();
  puts("PASS: production TTGO-only probe freshness, reset proof, heartbeat and bounded logging");
}
