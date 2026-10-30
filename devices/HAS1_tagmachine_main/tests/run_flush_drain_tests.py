#!/usr/bin/env python3
"""Exercise DiscardQueuedTags() against a modelled ESP32 UART driver.

Regression for the academy tag machine phantom tag (AT, 2026-10-30): door effects
block loop() for ~7.6 s while a held card keeps the Beetle streaming, the 256-byte
RX ring overflowed, and the UART driver re-injected its stashed overflow chunk only
after the post-lock flush had already read `available()` bytes once. The leftover
stale frames were dispatched on the next loop as a brand-new tag 1.3-1.6 s after
the door relocked (tagger: one more intrusion alarm, player in easy mode: the door
reopened).

Production code under test (extracted verbatim from the sketch): DiscardQueuedTags
and its flush timing constants. The UART, clock, frame parser and link state are
doubles. The serial double mirrors the driver contract that matters: bytes the
driver could not store while the ring was full come back only after a read frees
space, and a live Beetle produces one frame every ~50 ms.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(file, name):
    source = (ROOT / file).read_text(encoding="utf-8")
    match = re.search(r"(?:static )?(?:void|bool)\s+" + name + r"\s*\([^)]*\)\s*\{", source)
    if not match:
        raise RuntimeError(f"Missing firmware function: {name} in {file}")
    pos, depth = match.end(), 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[match.start():pos]


def constant(file, name):
    source = (ROOT / file).read_text(encoding="utf-8")
    match = re.search(r"const unsigned long " + name + r"\s*=\s*(\d+);", source)
    if not match:
        raise RuntimeError(f"Missing firmware constant: {name} in {file}")
    return int(match.group(1))


QUIET_MS = constant("serial_communication.ino", "BEETLE_FLUSH_QUIET_MS")
MAX_MS = constant("serial_communication.ino", "BEETLE_FLUSH_MAX_MS")
RX_BUFFER = int(re.search(r"#define BEETLE_RX_BUFFER_BYTES (\d+)",
                          (ROOT / "HAS1_tagmachine_main.h").read_text(encoding="utf-8")).group(1))

production = function("serial_communication.ino", "DiscardQueuedTags")

source = r'''
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

// ---- Arduino / clock double ----
static unsigned long now_ms = 0;
unsigned long millis() { return now_ms; }
void delay(unsigned long ms) { now_ms += ms; }
struct Log { template <class T> void println(const T &) {} } Serial;
static bool Elapsed(unsigned long now, unsigned long since, unsigned long interval) {
  return (unsigned long)(now - since) >= interval;
}
const unsigned long BEETLE_FLUSH_QUIET_MS = %(quiet)d;
const unsigned long BEETLE_FLUSH_MAX_MS = %(max)d;
static bool otaActive = false;
bool BeetleOtaActive() { return otaActive; }

// ---- UART driver double ----
// `ring` is what available()/read() expose. `stash` is the chunk the driver could
// not store while the ring was full; the real driver pushes it into the ring the
// first time a read frees space, so it is invisible to a single available()
// snapshot. A live card adds one frame every live_period_ms of simulated time.
static const std::string FRAME = "G9P1\r\n";
struct FakeSerial {
  std::deque<char> ring;
  std::string stash;
  int live_period_ms = 0;
  unsigned long next_live = 0;
  int reads = 0;
  void push_frames(int count) { for (int i = 0; i < count; i++) for (char c : FRAME) ring.push_back(c); }
  void produce() {
    if (live_period_ms <= 0) return;
    while (now_ms >= next_live) { for (char c : FRAME) ring.push_back(c); next_live += live_period_ms; }
  }
  int available() { produce(); return (int)ring.size(); }
  int read() {
    produce();
    if (ring.empty()) return -1;
    char c = ring.front(); ring.pop_front(); reads++;
    if (!stash.empty()) { for (char s : stash) ring.push_back(s); stash.clear(); }
    return c;
  }
};

// ---- link state double (only the members the flush touches) ----
struct BeetleLinkState {
  FakeSerial *serial;
  const char *name;
  char rxLine[64];
  unsigned char rxLength;
  bool rxOverflow;
  char pendingTag[5];
  bool pendingTagValid;
  bool pendingRelayPulse;
  unsigned droppedInputCount;
  unsigned tagCount;
};
enum { BEETLE_SUB = 0, BEETLE_MAIN, BEETLE_COUNT };
BeetleLinkState beetleLinks[BEETLE_COUNT] = {};

// Frame parser double: same byte budget contract as the sketch, a line of four or
// more characters becomes the single pending tag.
static void ReadBeetleFramesWithBudget(int idx, size_t byteBudget) {
  BeetleLinkState &link = beetleLinks[idx];
  size_t bytesRead = 0;
  while (link.serial->available() > 0 && bytesRead < byteBudget) {
    const int value = link.serial->read();
    if (value < 0) break;
    bytesRead++;
    const char incoming = (char)value;
    if (incoming == '\r') continue;
    if (incoming == '\n') {
      if (link.rxLength >= 4) { link.pendingTagValid = true; link.tagCount++; }
      link.rxLength = 0;
      continue;
    }
    if (link.rxLength < sizeof(link.rxLine) - 1) link.rxLine[link.rxLength++] = incoming;
  }
}

static void DiscardQueuedTags(int idx);

// PRODUCTION

// ---- scenarios ----
static FakeSerial port;
static void reset(int queued_frames, int stashed_frames, int live_period_ms) {
  now_ms = 1000;
  otaActive = false;
  port = FakeSerial();
  port.push_frames(queued_frames);
  for (int i = 0; i < stashed_frames; i++) port.stash += FRAME;
  port.live_period_ms = live_period_ms;
  port.next_live = now_ms + live_period_ms;
  beetleLinks[BEETLE_MAIN] = BeetleLinkState();
  beetleLinks[BEETLE_MAIN].serial = &port;
  beetleLinks[BEETLE_MAIN].name = "Main";
}

static void legacy_single_read_flush(int idx) {
  // The pre-fix flush: one available()-sized read.
  BeetleLinkState &link = beetleLinks[idx];
  const int queuedBytes = link.serial->available();
  if (queuedBytes > 0) ReadBeetleFramesWithBudget(idx, (size_t)queuedBytes);
  link.pendingTagValid = false;
}

int main() {
  // 1. The regression itself: the legacy flush leaves the re-injected stash behind.
  reset(40, 10, 0);
  legacy_single_read_flush(BEETLE_MAIN);
  assert(port.available() == 10 * (int)FRAME.size() && "legacy flush must miss the stash (documents the bug)");

  // 2. Fixed flush consumes the stash as well and discards the stale pending tag.
  reset(40, 10, 0);
  const unsigned long t0 = now_ms;
  DiscardQueuedTags(BEETLE_MAIN);
  assert(port.available() == 0 && port.stash.empty());
  assert(!beetleLinks[BEETLE_MAIN].pendingTagValid);
  assert(beetleLinks[BEETLE_MAIN].droppedInputCount == 1);
  assert(now_ms - t0 <= BEETLE_FLUSH_MAX_MS);
  assert(now_ms - t0 >= BEETLE_FLUSH_QUIET_MS && "waits out the quiet window after reading");

  // 3. An empty port returns immediately: nothing read, nothing can resurface.
  reset(0, 0, 0);
  const unsigned long t1 = now_ms;
  DiscardQueuedTags(BEETLE_MAIN);
  assert(now_ms == t1 && port.reads == 0);
  assert(beetleLinks[BEETLE_MAIN].droppedInputCount == 0);

  // 4. A card still on the reader keeps streaming every 50 ms. The flush must
  //    still return inside the hard bound and leave the live card detectable.
  reset(2, 0, 50);
  const unsigned long t2 = now_ms;
  DiscardQueuedTags(BEETLE_MAIN);
  assert(now_ms - t2 <= BEETLE_FLUSH_MAX_MS);
  assert(now_ms - t2 >= BEETLE_FLUSH_QUIET_MS);
  delay(60);
  assert(port.available() > 0 && "a live card is still seen after the flush");

  // 5. Beetle OTA keeps the historical single pass (no quiet wait).
  reset(10, 5, 0);
  otaActive = true;
  const unsigned long t3 = now_ms;
  DiscardQueuedTags(BEETLE_MAIN);
  assert(now_ms == t3);
  assert(!beetleLinks[BEETLE_MAIN].pendingTagValid);

  std::puts("flush drain tests passed");
  return 0;
}
'''

source = (source % {"quiet": QUIET_MS, "max": MAX_MS}).replace("// PRODUCTION", production)

if RX_BUFFER < 1024:
    raise SystemExit(f"BEETLE_RX_BUFFER_BYTES={RX_BUFFER} cannot hold a ~8 s blind window of Beetle frames")

with tempfile.TemporaryDirectory() as tmp:
    cpp = Path(tmp) / "flush_drain_test.cpp"
    exe = Path(tmp) / "flush_drain_test"
    cpp.write_text(source, encoding="utf-8")
    subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
