#include "../peer_state.h"

#include <assert.h>
#include <stdio.h>

using namespace iotglove;

static wire::Frame frame(const char* type, uint32_t id, const char* a, const char* b,
                         const char* c = nullptr, const char* d = nullptr) {
  wire::Frame f;
  strcpy(f.type, type); f.id = id;
  assert(wire::put(f, a)); assert(wire::put(f, b));
  if (c) assert(wire::put(f, c));
  if (d) assert(wire::put(f, d));
  return f;
}

int main() {
  PeerState peer;
  auto loc = frame("LOC", 1, "living", "-60", "100", "1");
  peer.receive(loc, 100); assert(!peer.locationFresh(100));
  assert(peer.receive(frame("HELLO", 0, "beetle", "1", "1", "10"), 200));
  peer.receive(loc, 300);
  assert(peer.locationFresh(300) && strcmp(peer.room(300), "living") == 0);
  assert(peer.locationFresh(5199));
  peer.receive(loc, 5199);  // duplicates must not extend location lifetime
  assert(!peer.locationFresh(5200));
  peer.receive(frame("LOC", 2, "unknown", "-60", "0", "1"), 5300);
  assert(!peer.locationFresh(5300));
  assert(peer.beginOta(7, 5301));
  peer.receive(frame("OTA_RESULT", 8, "skipped", "1"), 5302);
  assert(peer.ota() == PeerOta::Waiting);  // stale request must not unlock TTGO
  peer.receive(frame("OTA_RESULT", 7, "flashing", "1"), 5303);
  assert(peer.ota() == PeerOta::Waiting);
  peer.receive(frame("HELLO", 0, "beetle", "2", "1", "11"), 5400);
  assert(peer.ota() == PeerOta::Waiting);  // reboot alone is not update proof
  peer.receive(frame("OTA_RESULT", 7, "updated", "2"), 5401);
  assert(peer.ota() == PeerOta::Ready);
  peer.clearOta(); assert(peer.beginOta(9, 5402));
  peer.receive(frame("OTA_RESULT", 9, "updated", "2"), 5403);
  assert(peer.ota() == PeerOta::Failed);  // claimed update did not change version
  peer.clearOta(); assert(peer.beginOta(10, 5404));
  peer.tick(5404 + kBeetleOtaTimeoutMs);
  assert(peer.ota() == PeerOta::Failed);
  peer.clearOta();
  peer.receive(frame("HELLO", 0, "beetle", "2", "1", "11"), 400000);
  assert(peer.beginOta(12, 400000));
  peer.receive(frame("HEART", 0, "400001", "0", "1"), 400001);
  assert(peer.busy(400001));
  // Stale busy heartbeat remains protected through the OTA deadline only.
  peer.receive(frame("HELLO", 0, "beetle", "2", "1", "11"), 699999);
  assert(peer.busy(699999));
  peer.tick(700000);
  assert(peer.ota() == PeerOta::Failed && !peer.busy(700000));
  // Explicit rollback is allowed only for the exact target with reboot proof.
  PeerState rollback;
  rollback.receive(frame("HELLO", 0, "beetle", "12", "1", "100"), 0);
  assert(rollback.beginOta(21, 1, 7));
  assert(!rollback.beginOta(22, 2, 8));  // active target cannot be overwritten
  rollback.receive(frame("OTA_RESULT", 21, "updated", "7"), 3);
  assert(rollback.ota() == PeerOta::Waiting);  // result before boot proof is insufficient
  rollback.receive(frame("HELLO", 0, "beetle", "7", "1", "101"), 4);
  rollback.receive(frame("OTA_RESULT", 21, "updated", "7"), 5);
  assert(rollback.ota() == PeerOta::Ready);
  rollback.clearOta();
  assert(rollback.beginOta(22, 6, 7));
  rollback.receive(frame("OTA_RESULT", 22, "skipped", "7"), 7);
  assert(rollback.ota() == PeerOta::Ready);
  rollback.clearOta();
  assert(rollback.beginOta(23, 8, 4));
  rollback.receive(frame("OTA_RESULT", 23, "skipped", "7"), 9);
  assert(rollback.ota() == PeerOta::Failed);  // existing version is not requested target
  rollback.clearOta();
  assert(rollback.beginOta(24, 10, 4));
  rollback.receive(frame("HELLO", 0, "beetle", "3", "1", "102"), 11);
  rollback.receive(frame("OTA_RESULT", 24, "updated", "3"), 12);
  assert(rollback.ota() == PeerOta::Failed);  // wrong lower version is not a rollback success
  rollback.clearOta();
  assert(rollback.beginOta(25, 13));  // unpinned legacy latest permits a changed version
  rollback.receive(frame("HELLO", 0, "beetle", "2", "1", "103"), 14);
  rollback.receive(frame("OTA_RESULT", 25, "updated", "2"), 15);
  assert(rollback.ota() == PeerOta::Ready);
  rollback.receive(frame("HELLO", 0, "beetle", "3", "1", "104"), 16);
  assert(rollback.ota() == PeerOta::Failed);  // latest proof is invalidated too

  PeerState readyProof;
  readyProof.receive(frame("HELLO", 0, "beetle", "12", "1", "200"), 0);
  assert(readyProof.beginOta(30, 1, 7));
  readyProof.receive(frame("HELLO", 0, "beetle", "7", "1", "201"), 2);
  readyProof.receive(frame("OTA_RESULT", 30, "updated", "7"), 3);
  assert(readyProof.ota() == PeerOta::Ready);
  readyProof.receive(frame("HELLO", 0, "beetle", "12", "1", "202"), 4);
  assert(readyProof.ota() == PeerOta::Failed);  // target proof cannot outlive a firmware change
  readyProof.clearOta();
  assert(readyProof.beginOta(31, 5, 7));
  readyProof.receive(frame("HELLO", 0, "beetle", "7", "1", "203"), 6);
  readyProof.receive(frame("OTA_RESULT", 31, "updated", "7"), 7);
  assert(readyProof.ota() == PeerOta::Ready);
  readyProof.receive(frame("HELLO", 0, "beetle", "7", "1", "204"), 8);
  assert(readyProof.ota() == PeerOta::Ready);  // another boot with the same target remains valid
  readyProof.tick(8 + kLocationFreshMs - 1);
  assert(readyProof.ota() == PeerOta::Ready);
  readyProof.tick(8 + kLocationFreshMs);
  assert(readyProof.ota() == PeerOta::Failed);  // no TTGO update after peer goes offline
  puts("PASS: production UART peer freshness, room validation and OTA ordering");
}
