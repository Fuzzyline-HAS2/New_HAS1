#!/usr/bin/env python3
"""Exercise production RFID presence/gain logic with host PN532 and callback doubles.

Only RfidLoop's local buffer is instrumented: bytes beyond the actual four-byte
page (if any) get different poison on each poll. This deterministically exposes
the original 32-byte comparison without depending on undefined stack contents.
CardChecking is a callback double; HTTP, rewards and physical RF are not tested.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"(?:static )?(?:void|bool)\s+" + name + r"\s*\([^)]*\)\s*\{", source)
    if not match:
        raise RuntimeError(f"Missing firmware function: {name}")
    pos, depth = match.end(), 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[match.start():pos]


header = (ROOT / "HAS1_altar.h").read_text()
sensor = (ROOT / "sensor.ino").read_text()
constants = "\n".join(line for line in header.splitlines() if
                      line.startswith(("enum GainMode", "static constexpr size_t RFID_PAGE_DATA_SIZE",
                                       "#define TAG_REMOVE_TIME_MS")))
state = "\n".join(line for line in sensor.splitlines() if
                  re.match(r"static (?:GainMode|bool|uint8_t|unsigned long) pn532_", line)
                  or re.match(r"static bool (?:tag_active|chip_return_processed|tag_data_fetched) =", line))
loop = function(sensor, "RfidLoop")
loop, count = re.subn(r"(uint8_t data\[[^]]+\](?:\s*=\s*\{0\})?;)",
                      r"\1\n  for (size_t i = 4; i < sizeof(data); ++i) data[i] = ++poison;", loop)
assert count == 1, "Expected exactly one local card buffer"
source = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
using byte = uint8_t;
#define BREADCRUMB(x) ((void)0)
#define PN532_MIFARE_ISO14443A 0
unsigned long now_ms = 0;
unsigned long millis() { return now_ms; }
uint8_t poison = 0;
struct SerialDouble { void println(const char *) {} } Serial;
bool rfid_tag = false;
int rfid_timer_id = 0;
void RfidTagTimerFunc() { rfid_tag = false; }
struct Timer { int setTimeout(int, void (*)()) { return 1; } } rfid_timer;
struct Read { bool ack; bool detect; bool success; std::string data; };
std::deque<Read> reads;
int gains_applied = 0;
struct Reader {
  bool sendCommandCheckAck(uint8_t *, size_t len, int = 0) {
    if (len != 1) { ++gains_applied; return true; }
    assert(!reads.empty());
    if (!reads.front().ack) { reads.pop_front(); return false; }
    return true;
  }
  bool startPassiveTargetIDDetection(int) {
    assert(!reads.empty());
    if (!reads.front().detect) { reads.pop_front(); return false; }
    return true;
  }
  bool ntag2xx_ReadPage(int page, uint8_t *out) {
    assert(page == 7 && !reads.empty());
    Read result = reads.front(); reads.pop_front();
    // Failed transactions can modify the buffer too: caller must ignore it.
    memcpy(out, result.data.data(), 4);
    return result.success;
  }
} nfc;
void CardChecking(uint8_t *);
// PRODUCTION
int card_checks = 0, fetches = 0, returns = 0;
std::string checked_code;
void CardChecking(uint8_t *data) {
  ++card_checks;
  checked_code.assign(reinterpret_cast<char *>(data), 4);
  tag_active = true;
  if (!tag_data_fetched) { tag_data_fetched = true; ++fetches; }
  if (!chip_return_processed) { chip_return_processed = true; ++returns; }
}
Read card(const char *code = "G1P2") { return {true, true, true, code}; }
Read miss() { return {true, true, false, "XXXX"}; }
void poll(unsigned long at, std::initializer_list<Read> attempts) {
  assert(reads.empty());
  now_ms = at; reads = attempts;
  RfidTagTimerFunc(); RfidLoop();
  assert(reads.empty());
}
void reset() {
  reads.clear(); pn532_gain = GAIN_NEAR; pn532_tag_locked = false;
  pn532_last_seen_ms = 0; tag_active = chip_return_processed = tag_data_fetched = false;
  card_checks = fetches = returns = gains_applied = 0;
}
int main() {
  // Same code over many polls keeps all session gates set despite changing tail poison.
  reset();
  for (int i = 0; i < 20; ++i) poll(i * 1000, {card()});
  assert(pn532_tag_locked && tag_active && chip_return_processed && tag_data_fetched);
  assert(card_checks == 20 && returns == 1 && fetches == 1 && gains_applied == 0);
  // Poll throttle prevents an extra card read/callback.
  RfidLoop(); assert(card_checks == 20);

  // Failure inside grace period preserves gates; successful retry extends last-seen.
  poll(20000, {miss(), miss()});
  assert(pn532_tag_locked && chip_return_processed && tag_data_fetched && card_checks == 20);
  poll(21000, {miss(), card()});
  assert(pn532_gain == GAIN_FAR && pn532_last_seen_ms == 21000);
  assert(returns == 1 && fetches == 1);
  poll(23499, {miss(), miss()});
  assert(pn532_tag_locked && tag_active && chip_return_processed && tag_data_fetched);
  poll(23500, {miss(), miss()});
  assert(!pn532_tag_locked && !tag_active && !chip_return_processed && !tag_data_fetched);
  assert(pn532_gain == GAIN_NEAR);
  poll(24500, {card()}); assert(returns == 2 && fetches == 2);

  // A genuinely different four-byte code must not sustain the old session.
  reset(); poll(0, {card()});
  poll(1000, {card("G1P3"), card("G1P3")});
  assert(pn532_tag_locked && card_checks == 1 && checked_code == "G1P2");
  poll(2500, {card("G1P3"), card("G1P3")});
  assert(!pn532_tag_locked && !chip_return_processed && !tag_data_fetched);
  poll(3500, {card("G1P3")});
  assert(checked_code == "G1P3" && fetches == 2 && returns == 2);

  // Search retries the other gain; command/detection failures never process stale data.
  reset(); poll(0, {{false, true, true, "G1P2"}, card()});
  assert(pn532_tag_locked && pn532_gain == GAIN_FAR && card_checks == 1);
  poll(1000, {{true, false, true, "G1P2"}, {false, true, true, "G1P2"}});
  assert(pn532_tag_locked && card_checks == 1 && chip_return_processed);
  poll(2500, {miss(), miss()}); assert(!pn532_tag_locked && card_checks == 1);
  reset(); poll(0, {miss(), miss()});
  assert(!pn532_tag_locked && card_checks == 0 && !tag_data_fetched);
  std::cout << "PASS: altar production RFID presence, retries, debounce, gates and card replacement\n";
}
'''.replace('// PRODUCTION', constants + '\n' + state + '\n' +
            function(sensor, 'ApplyGain') + '\n' + function(sensor, 'DetectAndRead') + '\n' + loop)
with tempfile.TemporaryDirectory(prefix='altar-rfid-') as tmp:
    src, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
    src.write_text(source)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
