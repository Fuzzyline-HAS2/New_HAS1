#include <assert.h>
#include "../beetle_ota_state.h"
#include "../ttgo_ota_record.h"

using namespace tagmachine;
using namespace tagmachine::ota_wire;

static Response version(uint32_t id, uint32_t fw, uint32_t partition, uint32_t boot) {
  Response value;
  value.type = ResponseType::Version;
  value.request = id;
  value.protocol = kProtocolVersion;
  value.firmware = fw;
  value.partition = partition;
  value.boot = boot;
  return value;
}

static Response outcome(uint32_t id, Outcome code, uint32_t fw,
                        uint32_t partition, uint32_t boot) {
  Response value;
  value.type = ResponseType::Outcome;
  value.request = id;
  value.outcome = code;
  value.firmware = fw;
  value.partition = partition;
  value.boot = boot;
  return value;
}

int main() {
  using namespace tagmachine::ttgo_ota_record;
  const Record pending = make(14, 13, 1, "github@14:4");
  assert(valid(pending));
  assert(bootProvesInstalled(pending, 14, 1));
  assert(!bootProvesInstalled(pending, 13, 1));
  assert(!bootProvesInstalled(pending, 14, 2));
  Record corrupt = pending;
  corrupt.command[0] ^= 1;
  assert(!valid(corrupt));

  assert(!freshReadyProof(true, 7, 7));  // stale A parsed in the same millisecond
  assert(!freshReadyProof(false, 7, 8));
  assert(freshReadyProof(true, 7, 8));

  BeetleOtaPeer peer;
  peer.discover(11, 100);
  assert(peer.receiveVersion(version(11, 4, 1, 20), 101));
  assert(peer.state() == BeetleOtaState::Capable);
  assert(peer.begin(102, 1));
  assert(peer.receiveOutcome(outcome(11, Outcome::Accepted, 4, 1, 20), 103));
  assert(peer.progressSeen());
  assert(peer.receiveOutcome(outcome(11, Outcome::Flashing, 4, 1, 20), 104));
  assert(peer.state() == BeetleOtaState::Waiting);
  // A terminal result alone is insufficient; require a matching post-begin RV.
  assert(peer.receiveOutcome(outcome(11, Outcome::Updated, 5, 1, 21), 105));
  assert(peer.state() == BeetleOtaState::Waiting);
  assert(peer.receiveVersion(version(11, 5, 1, 21), 106));
  assert(peer.state() == BeetleOtaState::Ready);

  BeetleOtaPeer skipped;
  skipped.discover(12, 0);
  skipped.receiveVersion(version(12, 5, 1, 30), 1);
  assert(skipped.begin(2, 1));
  skipped.receiveOutcome(outcome(12, Outcome::Skipped, 5, 1, 30), 3);
  assert(skipped.state() == BeetleOtaState::Ready);

  BeetleOtaPeer skippedAfterReset;
  skippedAfterReset.discover(120, 0);
  skippedAfterReset.receiveVersion(version(120, 5, 1, 30), 1);
  assert(skippedAfterReset.begin(2, 1));
  // Simulate reset after durable Skipped but before its original UART event.
  skippedAfterReset.receiveOutcome(outcome(120, Outcome::Skipped, 5, 1, 31), 3);
  assert(skippedAfterReset.state() == BeetleOtaState::Waiting);
  skippedAfterReset.receiveVersion(version(120, 5, 1, 31), 4);
  assert(skippedAfterReset.state() == BeetleOtaState::Ready);

  BeetleOtaPeer pinned;
  pinned.discover(121, 0);
  pinned.receiveVersion(version(121, 4, 1, 32), 1);
  assert(pinned.begin(2, 1, 6));
  pinned.receiveOutcome(outcome(121, Outcome::Updated, 5, 1, 33), 3);
  assert(pinned.state() == BeetleOtaState::Failed);

  BeetleOtaPeer pinnedSkipMismatch;
  pinnedSkipMismatch.discover(122, 0);
  pinnedSkipMismatch.receiveVersion(version(122, 5, 1, 34), 1);
  assert(pinnedSkipMismatch.begin(2, 1, 6));
  pinnedSkipMismatch.receiveOutcome(outcome(122, Outcome::Skipped, 5, 1, 34), 3);
  assert(pinnedSkipMismatch.state() == BeetleOtaState::Failed);

  BeetleOtaPeer badProof;
  badProof.discover(13, 0);
  badProof.receiveVersion(version(13, 5, 1, 40), 1);
  assert(badProof.begin(2, 1));
  badProof.receiveOutcome(outcome(13, Outcome::Updated, 5, 1, 41), 3);
  assert(badProof.state() == BeetleOtaState::Failed);

  BeetleOtaPeer timedOut;
  timedOut.discover(14, UINT32_MAX - 10U);
  timedOut.tick(5, 20, 100);
  assert(timedOut.state() == BeetleOtaState::Discovering);
  timedOut.tick(10, 20, 100);
  assert(timedOut.state() == BeetleOtaState::Failed);

  BeetleOtaPeer uncertain;
  uncertain.discover(15, 0);
  uncertain.receiveVersion(version(15, 4, 1, 50), 1);
  assert(uncertain.begin(2, 1));
  uncertain.tick(103, 20, 100);
  assert(uncertain.state() == BeetleOtaState::InDoubt);
  // A late, fully proven terminal result can still resolve an in-doubt flash.
  uncertain.receiveOutcome(outcome(15, Outcome::Updated, 5, 1, 51), 104);
  uncertain.receiveVersion(version(15, 5, 1, 51), 105);
  assert(uncertain.state() == BeetleOtaState::Ready);

  BeetleOtaSequence sequence;
  sequence.start();
  assert(sequence.stage() == BeetleOtaStage::DiscoverMain);
  sequence.discovered();
  assert(sequence.stage() == BeetleOtaStage::UpdateMain);
  sequence.completed();
  assert(sequence.stage() == BeetleOtaStage::DiscoverSub);
  sequence.discovered();
  assert(sequence.stage() == BeetleOtaStage::UpdateSub);
  assert(sequence.retryCurrent());
  assert(sequence.stage() == BeetleOtaStage::DiscoverSub);
  sequence.discovered();
  sequence.completed();
  assert(sequence.stage() == BeetleOtaStage::ReadyForTtgo);
  return 0;
}
