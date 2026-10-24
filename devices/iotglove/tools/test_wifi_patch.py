"""Exercise the real additive fixed-SSID method with synthetic credentials/Wi-Fi."""

from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

PATCH = Path(__file__).with_name("has2-wifi-result-api.patch")


def patched_source():
    patch = PATCH.read_text(encoding="utf-8")
    section = patch.split("--- a/HAS2_Wifi/HAS2_Wifi.cpp\n", 1)[1]
    section = section.split("--- /dev/null\n", 1)[0]
    # Compile the production functions added by the patch, not a Python reimplementation.
    return "\n".join(line[1:] for line in section.splitlines()
                     if line.startswith(("+", " ")) and not line.startswith("+++"))


def fixed_method():
    return re.search(r"bool HAS2_Wifi::TrySetupFixed\([^)]*\)\n\{.*?\n\}",
                     patched_source(), re.S).group(0)


def checked_connect():
    # The status helpers and the attempt itself, in source order.
    return re.search(r"static bool StationIdle\(.*?\nbool HAS2_Wifi::TryConnectChecked\([^)]*\)\n\{.*?\n\}",
                     patched_source(), re.S).group(0)


HARNESS = r'''
#include <cassert>
#include <string>
using String = std::string;
enum { WIFI_STA, WL_DISCONNECTED, WL_CONNECTED };
struct FakeWiFi {
  int connection = WL_DISCONNECTED;
  bool persistentEnabled = true, autoReconnectEnabled = true, station = false;
  unsigned disconnects = 0;
  String connectedSsid;
  void persistent(bool enabled) { persistentEnabled = enabled; }
  void setAutoReconnect(bool enabled) { autoReconnectEnabled = enabled; }
  void mode(int value) { assert(value == WIFI_STA); station = true; }
  String macAddress() { assert(station); return "synthetic-device-id"; }
  int status() const { return connection; }
  String SSID() const { return connectedSsid; }
  void disconnect(bool off, bool erase) {
    assert(off && erase); ++disconnects; connection = WL_DISCONNECTED; connectedSsid.clear();
  }
} WiFi;
struct HAS2_WifiCandidate { const char *ssid; const char *password; };
const int WIFI_COUNT = 3;
HAS2_WifiCandidate badland_wifi_candidates[WIFI_COUNT] = {
  {"synthetic-other-one", "synthetic-only-a"},
  {"badland_shoot", "synthetic-only-target"},
  {"synthetic-other-two", "synthetic-only-b"},
};
HAS2_WifiCandidate city_wifi_candidates[WIFI_COUNT] = {
  {"synthetic-city-one", "synthetic-only-c"},
  {"synthetic-city-two", "synthetic-only-d"},
  {"synthetic-city-three", "synthetic-only-e"},
};
String _activeHost;
class HAS2_Wifi {
 public:
  String _theme, HOST_NAME, server, my_mac, PHP_FILE_NAME = "/synthetic-endpoint";
  unsigned attempts = 0;
  const char *attemptSsid = nullptr, *attemptPassword = nullptr;
  unsigned long timeout = 0;
  bool succeed = false, connectedAfter = true;
  String differentSsid;
  bool TrySetupFixed(String theme, const char *ssid);
  bool TryConnectChecked(const char *ssid, const char *password, unsigned long timeoutMs) {
    assert(!WiFi.persistentEnabled && !WiFi.autoReconnectEnabled && WiFi.station);
    ++attempts; attemptSsid = ssid; attemptPassword = password; timeout = timeoutMs;
    // Real TryConnectChecked tears down the previous connection and uses these credentials.
    if (WiFi.connection == WL_CONNECTED) WiFi.disconnect(true, true);
    if (succeed) {
      WiFi.connection = connectedAfter ? WL_CONNECTED : WL_DISCONNECTED;
      WiFi.connectedSsid = differentSsid.empty() ? String(ssid) : differentSsid;
    }
    return succeed;
  }
  bool TryConnectSaved() { assert(false && "saved AP must not be selected"); return false; }
  bool TryConnectOrdered() { assert(false && "fallback AP must not be selected"); return false; }
  void ScanNetworks(bool = false) { assert(false && "SSID selection must not scan"); }
};
'''

CASES = r'''
static void clearWifi() { WiFi = FakeWiFi{}; _activeHost.clear(); }
static void context(const HAS2_Wifi &client) {
  assert(client._theme == "badland" && !client.HOST_NAME.empty());
  assert(_activeHost == client.HOST_NAME && client.server == client.HOST_NAME + client.PHP_FILE_NAME);
  assert(client.my_mac == "synthetic-device-id");
}
int main() {
  clearWifi();
  HAS2_Wifi success; success.succeed = true;
  assert(success.TrySetupFixed("badland", "badland_shoot"));
  context(success);
  assert(success.attempts == 1 && success.timeout == 10000);  // Fixed-SSID attempts outlast the 3s library default.
  assert(success.attemptSsid == badland_wifi_candidates[1].ssid);
  assert(success.attemptPassword == badland_wifi_candidates[1].password);
  assert(success.TrySetupFixed("badland", "badland_shoot") && success.attempts == 1);

  clearWifi(); WiFi.connection = WL_CONNECTED; WiFi.connectedSsid = "saved-different-ap";
  HAS2_Wifi wrong; wrong.succeed = true;
  assert(wrong.TrySetupFixed("badland", "badland_shoot"));
  assert(wrong.attempts == 1 && WiFi.disconnects == 1 && WiFi.SSID() == "badland_shoot");

  clearWifi();
  HAS2_Wifi failed;
  assert(!failed.TrySetupFixed("badland", "badland_shoot"));
  context(failed); assert(failed.attempts == 1);
  assert(!failed.TrySetupFixed("badland", "badland_shoot") && failed.attempts == 2);
  assert(String(failed.attemptSsid) == "badland_shoot");

  clearWifi();
  HAS2_Wifi wrongResult; wrongResult.succeed = true; wrongResult.differentSsid = "wrong-result-ap";
  assert(!wrongResult.TrySetupFixed("badland", "badland_shoot"));
  assert(wrongResult.attempts == 1 && WiFi.disconnects == 1 && WiFi.status() != WL_CONNECTED);

  clearWifi();
  HAS2_Wifi dropped; dropped.succeed = true; dropped.connectedAfter = false;
  assert(!dropped.TrySetupFixed("badland", "badland_shoot") && dropped.attempts == 1);

  for (const char *name : {"", "badland_shoot-extra", "BADLAND_SHOOT", "not-configured"}) {
    clearWifi(); HAS2_Wifi invalid;
    assert(!invalid.TrySetupFixed("badland", name) && invalid.attempts == 0);
  }
  clearWifi(); HAS2_Wifi invalid;
  assert(!invalid.TrySetupFixed("badland", nullptr));
  assert(!invalid.TrySetupFixed("unknown-theme", "badland_shoot"));
  assert(!invalid.TrySetupFixed("city", "badland_shoot") && invalid.attempts == 0);

  clearWifi(); HAS2_Wifi reordered; reordered.succeed = true;
  const auto selected = badland_wifi_candidates[1];
  badland_wifi_candidates[1] = badland_wifi_candidates[2]; badland_wifi_candidates[2] = selected;
  assert(reordered.TrySetupFixed("badland", "badland_shoot"));
  assert(reordered.attemptPassword == selected.password && reordered.attempts == 1);
}
'''


CHECKED_HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>
typedef enum {
  WL_NO_SSID_AVAIL = 1, WL_CONNECTED = 3, WL_CONNECT_FAILED = 4,
  WL_CONNECTION_LOST = 5, WL_DISCONNECTED = 6, WL_STOPPED = 254
} wl_status_t;
enum { WIFI_STA };
enum arduino_event_id_t { ARDUINO_EVENT_WIFI_STA_DISCONNECTED = 1 };
enum { WIFI_REASON_ASSOC_LEAVE = 8, WIFI_REASON_STA_LEAVING = 36 };
struct arduino_event_info_t { struct { uint8_t reason; } wifi_sta_disconnected; };
typedef int wifi_event_id_t;
typedef void (*EventHandler)(arduino_event_id_t, arduino_event_info_t);
EventHandler registeredHandler = nullptr;  // registrations outlive each fake station
unsigned registrations = 0;
unsigned long nowMs = 0;
unsigned long millis() { return nowMs; }
struct FakePrint { void print(const char *) {} void println(const char *) {} };
FakePrint debugPrint, *_has2DebugPrint = &debugPrint;
const int CONNECT = 0;  // script step that associates; any other value is a disconnect reason
using Script = std::vector<std::pair<unsigned long, int>>;
struct FakeWiFi {
  wl_status_t idle = WL_STOPPED;  // what this core reports once the station is off
  wl_status_t beginResult = WL_DISCONNECTED;
  Script script;                  // events at offsets after begin()
  size_t next = 0;
  long beganAt = -1;
  bool associated = false, sleep = true;
  unsigned begins = 0, teardowns = 0;
  void persistent(bool) {}
  void setAutoReconnect(bool) {}
  void mode(int) {}
  wifi_event_id_t onEvent(EventHandler handler, arduino_event_id_t event) {
    assert(event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    registeredHandler = handler;
    return wifi_event_id_t(++registrations);
  }
  void disconnect(bool off, bool erase) { assert(off && erase); ++teardowns; beganAt = -1; associated = false; }
  wl_status_t begin(const char *, const char *) { ++begins; beganAt = long(nowMs); next = 0; return beginResult; }
  wl_status_t status() const { return beganAt < 0 ? idle : associated ? WL_CONNECTED : WL_DISCONNECTED; }
  void setSleep(bool enabled) { sleep = enabled; }
  void deliverDue() {
    while (beganAt >= 0 && next < script.size() &&
           nowMs - static_cast<unsigned long>(beganAt) >= script[next].first) {
      const int step = script[next++].second;
      if (step == CONNECT) { associated = true; continue; }
      arduino_event_info_t info{};
      info.wifi_sta_disconnected.reason = uint8_t(step);
      registeredHandler(ARDUINO_EVENT_WIFI_STA_DISCONNECTED, info);
    }
  }
} WiFi;
void delay(unsigned long ms) { nowMs += ms; WiFi.deliverDue(); }
class HAS2_Wifi {
 public:
  unsigned saves = 0, prints = 0;
  void SaveLastWifi(const char *, const char *) { ++saves; }
  void PrintConnectedWifi() { ++prints; }
  bool TryConnectChecked(const char *new_ssid, const char *new_password, unsigned long timeoutMs);
};
'''

CHECKED_CASES = r'''
enum class Boot { First, Later };
// Returns the attempt's duration on the fake clock. Boot::First means no
// attempt outcome yet this boot, so Arduino's one retry is still to come.
static unsigned long attempt(Script script, bool connects, Boot boot = Boot::Later,
                             wl_status_t idle = WL_STOPPED, wl_status_t beginResult = WL_DISCONNECTED) {
  WiFi = FakeWiFi{}; WiFi.idle = idle; WiFi.beginResult = beginResult; WiFi.script = script;
  if (boot == Boot::First) stationOutcomes = 0;
  else if (stationOutcomes == 0) stationOutcomes = 1;
  nowMs = 5000;
  HAS2_Wifi client;
  assert(client.TryConnectChecked("synthetic-ssid", "synthetic-only", 10000) == connects);
  assert(WiFi.begins == 1 && registrations == 1);
  if (connects) assert(client.saves == 1 && client.prints == 1 && !WiFi.sleep && WiFi.teardowns == 1);
  else assert(client.saves == 0 && WiFi.teardowns == 2);
  return nowMs - 5000;
}
int main() {
  // A stopped station counts as idle: begin() comes at once, not after 1s.
  assert(attempt({{1400, CONNECT}}, true, Boot::First) <= 1450);
  // First outcome of a boot: Arduino's retry runs; the retry's own leave (36)
  // is no outcome, and the attempt ends with the retry's outcome, not at 10s.
  unsigned long t = attempt({{1400, 201}, {1400, 36}, {3800, 201}}, false, Boot::First);
  assert(t >= 3800 && t <= 3850);
  // A first failure that leaves a final-looking status still gets its retry.
  assert(attempt({{1400, 204}, {3800, CONNECT}}, true, Boot::First) <= 3850);
  t = attempt({{1490, 202}, {2500, 2}}, false, Boot::First);  // AUTH_EXPIRE changes no status
  assert(t >= 2500 && t <= 2550);
  // If the retry never reports, the attempt waits as it always did.
  t = attempt({{1400, 201}}, false, Boot::First);
  assert(t >= 10000 && t <= 10050);
  // Later attempts end with their first outcome, and teardown does not wait 1s.
  t = attempt({{3450, 201}}, false);
  assert(t >= 3450 && t <= 3500);
  assert(attempt({{1000, 36}, {1200, 8}, {4000, CONNECT}}, true) <= 4050);  // leaving is no outcome
  t = attempt({}, false);
  assert(t >= 10000 && t <= 10050);
  // A synchronous begin() failure does not idle at all.
  assert(attempt({}, false, Boot::Later, WL_STOPPED, WL_CONNECT_FAILED) <= 50);
  // The legacy idle status still ends the waits at once.
  assert(attempt({{1400, CONNECT}}, true, Boot::Later, WL_DISCONNECTED) <= 1450);
  // A station that never reports idle still bounds each teardown wait to 1s.
  t = attempt({{1400, CONNECT}}, true, Boot::Later, WL_CONNECT_FAILED);
  assert(t >= 2400 && t <= 2450);
  t = attempt({{1400, 201}}, false, Boot::Later, WL_CONNECT_FAILED);
  assert(t >= 3400 && t <= 3450);
}
'''


def compile_and_run(source, name):
    with tempfile.TemporaryDirectory(prefix=f"iotglove-{name}-test-") as work:
        path = Path(work) / f"{name}.cpp"
        binary = Path(work) / f"{name}_test"
        path.write_text(source)
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                        "-Werror", "-pedantic", str(path), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


class FixedSsidPatchTests(unittest.TestCase):
    def test_production_method_only_uses_requested_candidate(self):
        compile_and_run(HARNESS + "\n" + fixed_method() + "\n" + CASES, "fixed_ssid")

    def test_checked_attempt_ends_on_driver_failure(self):
        compile_and_run(CHECKED_HARNESS + "\n" + checked_connect() + "\n" + CHECKED_CASES,
                        "checked_connect")


if __name__ == "__main__":
    unittest.main()
