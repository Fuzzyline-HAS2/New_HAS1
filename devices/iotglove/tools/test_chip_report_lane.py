"""Exercise the production chip lane without Arduino, networking or hardware."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

GLOVE = Path(__file__).resolve().parents[1]


def production_functions():
    source = (GLOVE / "wifi.cpp").read_text()
    names = ("liveName", "readChipIdentity", "observePhysicalChip", "reportChip")
    functions = []
    for name in names:
        match = re.search(rf"(?:bool|void) {name}\([^)]*\) \{{", source)
        start, cursor, depth = match.start(), match.end(), 1
        while depth:
            char = source[cursor]
            depth += (char == "{") - (char == "}")
            cursor += 1
        functions.append(source[start:cursor])
    return "\n".join(functions)


HARNESS = r'''
#include "chip_report.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <map>
#include <string>
using namespace iotglove;
struct Value {
  std::string text;
  const char* operator|(const char*) const { return text.c_str(); }
};
struct Json {
  std::map<std::string, Value> values;
  Value& operator[](const char* key) { return values[key]; }
} my, server;
bool copyString(const Value& v, char* out, size_t size) {
  if (v.text.empty() || v.text.size() >= size) return false;
  strcpy(out, v.text.c_str()); return true;
}
bool number(const Value& v, long low, long high, long& out) {
  if (v.text.empty()) return false;
  char* end; out = strtol(v.text.c_str(), &end, 10);
  return !*end && out >= low && out <= high;
}
ChipReportPolicy chipReport;
std::atomic<uint8_t> physicalChip{2};
std::atomic<bool> forceSnapshot{false};
uint32_t now = 0;
uint32_t millis() { return now; }
void publishInvalid() {}
void remoteConsoleLogf(const char*, ...) {}
struct Wifi {
  unsigned writes = 0;
  bool apply = true, ready = true, transportAck = true, changeDuringWrite = false;
  bool ReceiveMineChecked() { my = server; return true; }
  bool SetGloveChipChecked(const char* name, bool present) {
    assert(server["device_name"].text == name);
    ++writes;
    if (apply) server["life_chip"].text = present ? "1" : "0";
    server["chip_report_ready"].text = ready ? "1" : "0";
    if (changeDuringWrite) physicalChip.store(!present);
    return transportAck;
  }
} wifi;
// PRODUCTION_FUNCTIONS
void reset(const char* state) {
  chipReport = ChipReportPolicy{}; wifi = Wifi{}; my = Json{}; server = Json{};
  now = 0; forceSnapshot.store(false); physicalChip.store(0);
  server["device_name"].text = "G1P1";
  server["device_state"].text = state;
  server["game_state"].text = "unrecognized-game-phase";
  server["role"].text = "unrecognized-role";
  server["life_chip"].text = "1";
  server["chip_report_ready"].text = "0";
  assert(readChipIdentity());
}
int main() {
  // These snapshots cannot decode as a valid game. The independent lane still
  // reports both physical values, then deduplicates the unchanged value.
  // The retired exploration value remains irrelevant to this independent physical lane.
  for (const char* state : {"unknown", "setting", "ready", "blink", "activate", "github", "ended", "photo", "exploration"}) {
    reset(state);
    reportChip(); assert(wifi.writes == 1 && server["life_chip"].text == "0");
    reportChip(); assert(wifi.writes == 1);
    physicalChip.store(1); reportChip(); assert(wifi.writes == 2 && server["life_chip"].text == "1");
    reportChip(); assert(wifi.writes == 2);
  }
  reset("unknown"); wifi.apply = false;
  reportChip(); assert(wifi.writes == 1);  // HTTP ACK alone cannot acknowledge 0.
  now = 4999; reportChip(); assert(wifi.writes == 1);
  now = 5000; wifi.apply = true; reportChip(); assert(wifi.writes == 2);
  reset("github"); wifi.ready = false;
  reportChip(); now = 5000; reportChip(); assert(wifi.writes == 2);  // Exact value alone is insufficient.
  reset("ready"); wifi.transportAck = false;
  reportChip(); reportChip(); assert(wifi.writes == 1);  // Lost HTTP ACK is resolved by fresh readback.
  server["chip_report_ready"].text = "0"; assert(readChipIdentity());
  reportChip(); assert(wifi.writes == 2);  // Server-only restart re-registers current state.
  reset("setting"); reportChip();
  server["life_chip"].text = "1"; assert(readChipIdentity());
  reportChip(); assert(wifi.writes == 2 && server["life_chip"].text == "0");
  reportChip(); assert(wifi.writes == 2);  // ready=1 DB correction is then deduplicated.
  server["life_chip"].text = ""; assert(readChipIdentity());
  reportChip(); assert(wifi.writes == 3 && server["life_chip"].text == "0");
  server["life_chip"].text = "2"; assert(readChipIdentity());
  reportChip(); assert(wifi.writes == 4 && server["life_chip"].text == "0");
  reset("activate"); wifi.changeDuringWrite = true;
  reportChip(); assert(wifi.writes == 1);
  wifi.changeDuringWrite = false;
  reportChip(); assert(wifi.writes == 2 && server["life_chip"].text == "1");
  reportChip(); assert(wifi.writes == 2);
  reset("ended"); server["device_name"].text = "G9P1";
  assert(!readChipIdentity()); reportChip(); assert(wifi.writes == 0);
}
'''


class ChipLaneTest(unittest.TestCase):
    def test_production_lane(self):
        with tempfile.TemporaryDirectory(prefix="iotglove-chip-lane-") as directory:
            source = Path(directory) / "lane.cpp"
            binary = Path(directory) / "lane"
            source.write_text(HARNESS.replace("// PRODUCTION_FUNCTIONS", production_functions()))
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-pedantic", "-I", str(GLOVE), str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
