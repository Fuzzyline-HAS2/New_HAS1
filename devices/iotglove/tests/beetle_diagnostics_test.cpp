#include <IoTGloveDiagnostics.h>
#include "peer_state.h"
#include <assert.h>
#include <string.h>

using namespace iotglove;
using namespace iotglove::diagnostics;

int main() {
  Log boot;
  boot.sequence = 1;
  boot.bootId = 1234;
  boot.value = 6;
  wire::Frame frame;
  assert(makeLogFrame(boot, frame));
  char text[wire::kMaxLine + 1];
  assert(wire::format(text, sizeof(text), frame));
  assert(strcmp(text, "IG1|LOG|1|1234|0|boot|reason|6\n") == 0);
  Log decoded;
  assert(parseLog(frame, decoded));
  assert(decoded.bootId == 1234 && decoded.value == 6 && decoded.event == Event::Boot);
  frame.id = 2;
  assert(!parseLog(frame, decoded));  // BOOT snapshot identity never changes.
  frame.id = 1;
  strcpy(frame.args[1], "1");
  assert(!parseLog(frame, decoded));  // BOOT uses origin uptime, not replay time.
  assert(makeLogFrame(boot, frame));
  strcpy(frame.args[3], "arbitrary-secret");
  assert(!parseLog(frame, decoded));
  assert(makeLogFrame(boot, frame));
  strcpy(frame.args[4], "secret");
  assert(!parseLog(frame, decoded));
  strcpy(frame.args[4], "4294967296");
  assert(!parseLog(frame, decoded));
  assert(makeLogFrame(boot, frame));
  frame.count = 6;
  assert(!parseLog(frame, decoded));

  Log reset;
  reset.sequence = 2;
  reset.bootId = 1234;
  reset.uptimeMs = 1000;
  reset.event = Event::Reset;
  reset.code = Code::Requested;
  reset.value = 1;
  assert(makeLogFrame(reset, frame) && parseLog(frame, decoded));
  reset.value = 3;
  assert(!makeLogFrame(reset, frame));
  reset.value = 1;
  reset.code = Code::Reason;
  assert(!makeLogFrame(reset, frame));

  Log ota;
  ota.sequence = UINT32_MAX;
  ota.bootId = 1234;
  ota.uptimeMs = UINT32_MAX;
  ota.event = Event::Ota;
  ota.value = UINT32_MAX;
  for (uint8_t code = static_cast<uint8_t>(Code::WifiStart);
       code <= static_cast<uint8_t>(Code::Disabled); ++code) {
    ota.code = static_cast<Code>(code);
    assert(makeLogFrame(ota, frame) && parseLog(frame, decoded));
    assert(decoded.value == UINT32_MAX && decoded.code == ota.code);
  }
  ota.value = 0;
  assert(!makeLogFrame(ota, frame));
  ota.value = 9;
  ota.code = static_cast<Code>(255);
  assert(!makeLogFrame(ota, frame));
  ota.code = Code::Checking;
  ota.event = static_cast<Event>(255);
  assert(!makeLogFrame(ota, frame));

  LogQueue queue;
  Log out;
  assert(!queue.pop(out));
  for (size_t i = 0; i < LogQueue::kCapacity; ++i) {
    boot.value = static_cast<uint32_t>(i);
    assert(queue.push(boot));
  }
  assert(!queue.push(boot) && queue.dropped() == 1 && queue.size() == 8);
  for (size_t i = 0; i < LogQueue::kDrainBudget; ++i) {
    assert(queue.pop(out));
    assert(out.value == i);
  }
  assert(queue.size() == 6);
  boot.value = 8; assert(queue.push(boot));
  boot.value = 9; assert(queue.push(boot));
  for (size_t i = 2; i < 10; ++i) {
    assert(queue.pop(out));
    assert(out.value == i);  // Wraparound preserves FIFO and the oldest records.
  }
  assert(!queue.pop(out) && queue.size() == 0 && queue.dropped() == 1);

  // Additive logs must not become peer/OTA proof to a receiver that ignores them.
  boot.value = 6;
  assert(makeLogFrame(boot, frame));
  PeerState peer;
  assert(!peer.receive(frame, 100) && !peer.known());
  wire::Frame hello;
  strcpy(hello.type, "HELLO");
  wire::put(hello, "beetle"); wire::put(hello, "1");
  wire::put(hello, "1"); wire::put(hello, "1234");
  assert(peer.receive(hello, 200));
  const uint32_t accepted = peer.acceptedFrames();
  assert(!peer.receive(frame, 400));
  assert(peer.lastSeenAge(400) == 200 && peer.acceptedFrames() == accepted);
}
