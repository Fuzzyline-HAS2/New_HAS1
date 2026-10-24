"""Exercise the real additive fixed-SSID method with synthetic credentials/Wi-Fi."""

from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

PATCH = Path(__file__).with_name("has2-wifi-result-api.patch")


def fixed_method():
    patch = PATCH.read_text()
    section = patch.split("--- a/HAS2_Wifi/HAS2_Wifi.cpp\n", 1)[1]
    section = section.split("--- /dev/null\n", 1)[0]
    # Compile the production function added by the patch, not a Python reimplementation.
    applied = "\n".join(line[1:] for line in section.splitlines()
                        if line.startswith(("+", " ")) and not line.startswith("+++"))
    return re.search(r"bool HAS2_Wifi::TrySetupFixed\([^)]*\)\n\{.*?\n\}", applied, re.S).group(0)


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
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 3000;
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
  bool TryConnect(const char *ssid, const char *password, unsigned long timeoutMs) {
    assert(!WiFi.persistentEnabled && !WiFi.autoReconnectEnabled && WiFi.station);
    ++attempts; attemptSsid = ssid; attemptPassword = password; timeout = timeoutMs;
    // Real TryConnect tears down the previous connection and uses these credentials.
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
  assert(success.attempts == 1 && success.timeout == WIFI_CONNECT_TIMEOUT_MS);
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


class FixedSsidPatchTests(unittest.TestCase):
    def test_production_method_only_uses_requested_candidate(self):
        source = HARNESS + "\n" + fixed_method() + "\n" + CASES
        with tempfile.TemporaryDirectory(prefix="iotglove-fixed-ssid-test-") as work:
            path = Path(work) / "fixed_ssid.cpp"
            binary = Path(work) / "fixed_ssid_test"
            path.write_text(source)
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                            "-Werror", "-pedantic", str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
