#!/usr/bin/env python3
"""Host regression for production RFID state machine and administrator latch.

Only hardware dependencies are mocked. The local scratch buffer is poisoned
before each scan to deterministically model changing stack contents; the PN532
mock writes exactly the four bytes provided by ntag2xx_ReadPage.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
rfid = (ROOT / 'rfid.ino').read_text()
rfid = re.sub(r'^#include .*\n', '', rfid, flags=re.M)
rfid, count = re.subn(
    r'(void RfidHalUpdate\(\)\s*\{\s*uint8_t data\[[^]]+\][^;]*;)',
    r'\1\n    memset(data, ++scratchPattern, sizeof(data));', rfid)
assert count == 1, 'RFID scratch buffer instrumentation must match once'
game = (ROOT / 'Game_system.ino').read_text()
start = game.index('static bool AdminCardToggle() {')
end = game.index('\n}\n', start) + 3
admin = game[start:end]
source = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include "hal.h"
using byte = uint8_t;
using String = std::string;
unsigned long nowMs = 0;
unsigned long millis() { return nowMs; }
void Log(const char*, const String&) {}
constexpr int PN532_SCK=0, PN532_MISO=0, PN532_MOSI=0, PN532_SS1=0;
constexpr int PN532_MIFARE_ISO14443A=0;
enum { GAME_CORRECT_ANIM, GAME_WRONG_ANIM, GAME_ITEM_FAIL_ANIM,
       GAME_TAGGER_ANIM, GAME_BOX_OPENING, GAME_READY };
int gameState = GAME_READY;
unsigned char scratchPattern = 0;
struct Reply { bool ok; const char* code; };
struct Adafruit_PN532 {
    Adafruit_PN532(int, int, int, int) {}
    std::vector<Reply> replies;
    size_t cursor = 0;
    void begin() {}
    int getFirmwareVersion() { return 1; }
    void SAMConfig() {}
    bool sendCommandCheckAck(uint8_t*, size_t, int = 0) { return true; }
    bool startPassiveTargetIDDetection(int) { return true; }
    bool ntag2xx_ReadPage(int page, uint8_t* out) {
        assert(page == 7 && cursor < replies.size());
        const Reply r = replies[cursor++];
        if (r.ok) memcpy(out, r.code, 4);
        else memset(out, 0xff, 4); // failed reads cannot publish partial data
        return r.ok;
    }
};
''' + rfid + r'''
bool opened = false, adminBoxOverride = false;
int toggles = 0;
bool isBoxOpened() { return opened; }
void boxOpen() { opened = true; ++toggles; }
void boxClose() { opened = false; ++toggles; }
''' + admin + r'''
void scan(unsigned long time, std::initializer_list<Reply> replies) {
    nowMs = time;
    nfc.replies = replies;
    nfc.cursor = 0;
    RfidHalUpdate();
    assert(nfc.cursor == nfc.replies.size());
}
void checkCode(const char* expected, bool consume) {
    uint8_t out[34]; memset(out, 0xa5, sizeof out);
    assert(consume ? RfidReadTag(out + 1) : RfidPeekTag(out + 1));
    assert(out[0] == 0xa5 && out[33] == 0xa5);
    assert(memcmp(out + 1, expected, 4) == 0);
    for (int i = 5; i < 33; ++i) assert(out[i] == 0);
}
int main() {
    RfidInit();
    scan(0, {{false, ""}, {false, ""}});
    assert(!RfidTagPresent());
    assert(!AdminCardToggle());
    // Discovery retries at the other gain and publishes deterministic HAL output.
    scan(100, {{false, ""}, {true, "G1P2"}});
    checkCode("G1P2", false); checkCode("G1P2", true);
    uint8_t out[32]; assert(!RfidReadTag(out));
    for (unsigned long t = 300; t < 2300; t += 200) {
        scan(t, {{true, "G1P2"}});
        assert(RfidTagPresent()); checkCode("G1P2", true);
    }
    // A successful fallback refreshes presence; a short complete failure does not remove.
    scan(2300, {{false, ""}, {true, "G1P2"}});
    checkCode("G1P2", true);
    scan(2599, {{false, ""}, {false, ""}});
    assert(RfidTagPresent() && !RfidReadTag(out));
    scan(2600, {{false, ""}, {false, ""}});
    assert(!RfidTagPresent() && !RfidReadTag(out));
    // Different page7 code is not published as the held card; removal then reacquisition.
    scan(3000, {{true, "G1P2"}}); checkCode("G1P2", true);
    scan(3100, {{true, "G2P3"}, {true, "G2P3"}});
    assert(RfidTagPresent() && !RfidPeekTag(out));
    scan(3300, {{true, "G2P3"}, {true, "G2P3"}});
    assert(!RfidTagPresent());
    scan(3500, {{true, "G2P3"}}); checkCode("G2P3", true);
    scan(3800, {{false, ""}, {false, ""}}); assert(!AdminCardToggle());
    // Actual production MMMM latch must toggle once even after >4 seconds of holding.
    scan(4000, {{true, "MMMM"}}); assert(AdminCardToggle() && toggles == 1);
    for (unsigned long t = 4200; t <= 9000; t += 200) {
        scan(t, {{true, "MMMM"}});
        assert(AdminCardToggle() && toggles == 1);
    }
    scan(9299, {{false, ""}, {false, ""}}); AdminCardToggle();
    assert(RfidTagPresent() && toggles == 1);
    scan(9300, {{false, ""}, {false, ""}}); assert(!AdminCardToggle());
    scan(9500, {{true, "MMMM"}}); assert(AdminCardToggle() && toggles == 2);
    puts("PASS: stable card, HAL output, gain fallback, debounce boundary, replacement, MMMM latch");
}
'''
with tempfile.TemporaryDirectory(prefix='itembox-rfid-') as tmp:
    cpp = Path(tmp) / 'test.cpp'
    binary = Path(tmp) / 'test'
    cpp.write_text(source)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(ROOT), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
