#include "../ota_request.h"
#include <assert.h>
#include <stdio.h>

using namespace iotglove;

static ServerSnapshot request(const char* text, uint32_t ttgo, uint32_t beetle, bool valid = true) {
  ServerSnapshot snapshot;
  snapshot.valid = true;
  snapshot.updateRequested = valid;
  strcpy(snapshot.updateCommand, text);
  snapshot.otaTtgoVersion = ttgo;
  snapshot.otaBeetleVersion = beetle;
  return snapshot;
}

int main() {
  OtaRequests queue;
  auto first = request("github@12:7", 12, 7);
  assert(!queue.observe(first) && queue.hasPending());
  assert(queue.pending().ttgoVersion == 12 && queue.pending().beetleVersion == 7);
  assert(queue.startPending());
  auto second = request("github@4:2", 4, 2);
  assert(!queue.observe(second));
  assert(queue.active().ttgoVersion == 12 && queue.active().beetleVersion == 7);
  assert(queue.pending().ttgoVersion == 4 && queue.pending().beetleVersion == 2);
  assert(!queue.startPending());
  queue.finishActive();
  assert(queue.startPending());
  assert(queue.active().ttgoVersion == 4 && queue.active().beetleVersion == 2);
  queue.finishActive();
  assert(!queue.observe(second) && !queue.hasPending());  // sticky command: no retry
  auto disconnected = second; disconnected.valid = false;
  queue.observe(disconnected);
  queue.observe(second);
  assert(!queue.hasPending());  // disconnect must not masquerade as a fresh command
  auto inactive = request("", 0, 0, false);
  queue.observe(inactive);
  queue.observe(second);
  assert(queue.hasPending());  // server clears/reissues same target deliberately
  auto malformed = request("github@oops", 0, 0, false);
  assert(queue.observe(malformed));
  assert(!queue.hasPending());  // malformed never falls back to latest
  assert(!queue.observe(malformed));
  queue.requestLatest();
  assert(queue.hasPending() && queue.pending().ttgoVersion == 0);
  queue.observe(inactive);
  assert(queue.hasPending());  // unrelated server state does not cancel USB request
  assert(queue.startPending());
  queue.observe(first);
  assert(queue.active().ttgoVersion == 0 && queue.pending().ttgoVersion == 12);
  queue.finishActive(); assert(queue.startPending());
  queue.observe(second);
  queue.observe(first);
  assert(!queue.hasPending());  // switching back to running target needs no duplicate

  OtaRequests phaseChange;
  phaseChange.observe(first); assert(phaseChange.startPending(100));
  // An actually-running Beetle is not interrupted, even after a game starts.
  assert(!phaseChange.abortReadyPair(101, false, false, true, Phase::Active));
  assert(phaseChange.hasActive());
  // As soon as Beetle is terminal, abort TTGO and release scan/reset/game locks.
  assert(phaseChange.abortReadyPair(102, true, false, true, Phase::Active));
  assert(!phaseChange.hasActive());
  phaseChange.observe(first); assert(!phaseChange.hasPending());  // no automatic retry
  phaseChange.observe(second); assert(phaseChange.startPending(200));
  assert(phaseChange.abortReadyPair(201, true, false, true, Phase::Photo));
  assert(!phaseChange.hasActive());

  OtaRequests offline;
  offline.observe(first);
  const uint32_t started = UINT32_MAX - 100U;
  assert(offline.startPending(started));
  assert(!offline.abortReadyPair(started + kBeetleOtaTimeoutMs - 1U,
      true, false, false, Phase::Setting));
  assert(offline.hasActive());
  assert(offline.abortReadyPair(started + kBeetleOtaTimeoutMs,
      true, false, false, Phase::Setting));  // unsigned rollover is handled
  assert(!offline.hasActive());

  OtaRequests submitted;
  submitted.observe(first); assert(submitted.startPending(0));
  assert(!submitted.abortReadyPair(kBeetleOtaTimeoutMs, false, false, true, Phase::Active));
  assert(!submitted.abortReadyPair(kBeetleOtaTimeoutMs, true, true, true, Phase::Active));
  assert(submitted.hasActive());  // never cancel a worker that may be writing flash

  puts("PASS: production OTA target latching, immutable board pair and malformed rejection");
}
