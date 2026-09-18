// Deterministic host fakes; the behavior under test comes from production .ino files.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include "production_constants.inc"

class String : public std::string {
 public:
  using std::string::string;
  String() = default;
  String(const char* value) : std::string(value ? value : "") {}
  String(const std::string& value) : std::string(value) {}
  template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
  String(T value) : std::string(std::to_string(value)) {}
};
struct Value {
  std::string text;
  Value& operator=(const char* value) { text = value; return *this; }
  Value& operator=(int value) { text = std::to_string(value); return *this; }
  operator const char*() const { return text.c_str(); }
  operator int() const { return text.empty() ? 0 : std::stoi(text); }
};
struct JsonDocument {
  std::map<std::string, Value> values;
  Value& operator[](const char* key) { return values[key]; }
} my, tag;

unsigned long fake_ms = 1000;
unsigned long millis() { return fake_ms; }
void delay(unsigned long ms) { fake_ms += ms; }
constexpr int HIGH = 1, LOW = 0, OUTPUT = 1;
std::vector<std::pair<unsigned long, int>> gpio_events;
void pinMode(int pin, int mode) { assert(pin == SOLENOID_PIN && mode == OUTPUT); }
void digitalWrite(int pin, int value) {
  assert(pin == SOLENOID_PIN);
  gpio_events.emplace_back(fake_ms, value);
}
struct SerialStub {
  std::vector<std::string> lines;
  void println(const std::string& message) { lines.push_back(message); }
} Serial;
struct WifiStub { int RSSI() { return -50; } } WiFi;
struct EspStub { uint32_t getFreeHeap() { return 100000; } } ESP;
struct OtaStub { void check() {} } ota;

bool activate_bool = false, ghost_open_pending = false;
unsigned long ghost_tag_start_ms = 0, ghost_situation_ms = 0, ghost_role_receive_ms = 0;
int ghost_poll_count = 0, ghost_rssi_at_tag = 0;
String last_open_tag_user;
constexpr unsigned long GHOST_OPEN_TIMEOUT_MS = 15000;
int white[3], red[3], yellow[3], blue[3], purple[3];
unsigned long poll_interval = 0;
int* displayed_color = nullptr;
void NeoNo() {}
void (*NeoFunc)() = NeoNo;
void NeopixelSet(int* color) { displayed_color = color; delay(10); }
void SetWifiPollInterval(unsigned long value) { poll_interval = value; }
void RfidLoop() {}
void BleAdvertiserUpdateFromDeviceName(const char*) {}
void SetBrightness(int) {}
void SolenoidInit();
void SolenoidOn();
void SolenoidOff();
void SolenoidPulse();
unsigned long SolenoidPulse(unsigned long);
void NeoBlinkPurple(int);
void DataChange();

struct Has2WifiStub {
  unsigned receive_calls = 0, situation_calls = 0, mine_calls = 0, send_calls = 0;
  unsigned long role_delay_ms = 300, situation_delay_ms = 200, approval_delay_ms = 400;
  bool situation_ok = true, approve = true;
  std::string role = "ghost";
  int already_open = 0;
  void Receive(const String&) {
    ++receive_calls;
    fake_ms += role_delay_ms;
    tag["role"] = role.c_str();
    tag["is_open"] = already_open;
  }
  bool Situation(const String&, const String&) {
    ++situation_calls;
    fake_ms += situation_delay_ms;
    return situation_ok;
  }
  void ReceiveMine() {
    ++mine_calls;
    fake_ms += approval_delay_ms;
    if (approve) my["device_state"] = "open";
  }
  void Send(const String&, const String& field, const String& value) {
    assert(field == "is_open" && value == "1");
    ++send_calls;
  }
} has2wifi;

#define _HAS1_REVIVAL_MACHINE_H_
#include "game_state.ino"
#include "sensor_under_test.inc"

void prepare(const char* game_state = "activate", const char* device_state = "activate") {
  my["device_name"] = "revival_machine_original";
  my["game_state"] = game_state;
  my["device_state"] = "activate";
  my["brightness"] = 50;
  DataChange();
  my["device_state"] = device_state;
  gpio_events.clear();
  Serial.lines.clear();
  fake_ms = 1000;
}
void card(const char* name = "G1P1") {
  uint8_t data[32] = {};
  std::memcpy(data, name, 4);
  CardChecking(data);
}
unsigned on_count() {
  unsigned count = 0;
  for (const auto& event : gpio_events) count += event.second == HIGH;
  return count;
}
unsigned long assert_pulse(unsigned long duration = 5000) {
  assert(on_count() == 1);
  auto found = gpio_events.begin();
  while (found->second != HIGH) ++found;
  const auto on = *found++;
  assert(found != gpio_events.end() && found->second == LOW);
  assert(found->first - on.first == duration);
  return on.first;
}
void assert_no_game_request() {
  assert(has2wifi.receive_calls == 0 && has2wifi.situation_calls == 0 &&
         has2wifi.mine_calls == 0 && has2wifi.send_calls == 0);
}
std::string unique_log(const std::string& prefix) {
  std::string result;
  unsigned count = 0;
  for (const auto& line : Serial.lines) {
    if (line.find(prefix) == 0) { result = line; ++count; }
  }
  assert(count == 1);
  return result;
}
void assert_timing_log(unsigned long started, unsigned long opened) {
  const std::string prefix = "[GhostTiming] RELAY ON: ";
  const std::string line = unique_log(prefix);
  assert(std::stoul(line.substr(prefix.size())) == opened - started);
  assert(line.find("role_receive=300ms") != std::string::npos);
  assert(line.find("situation=200ms") != std::string::npos);
}

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string scenario = argv[1];
  static_assert(SOLENOID_REVIVAL_PULSE_MS == 5000, "Gameplay pulse must stay five seconds");
  if (scenario == "approved") {
    prepare();
    const unsigned long started = millis();
    card();
    const unsigned long opened = assert_pulse();
    assert(opened - started >= 900 && opened - started <= 920);
    assert(ghost_role_receive_ms == 300);
    assert(!ghost_open_pending && has2wifi.send_calls == 1);
    assert_timing_log(started, opened);
    std::cout << "relay=" << opened - started << "ms ";
  } else if (scenario == "http200_without_open" || scenario == "situation_failure" || scenario == "deferred_approval") {
    prepare();
    has2wifi.approve = false;
    has2wifi.situation_ok = scenario != "situation_failure";
    card();
    assert(on_count() == 0 && has2wifi.send_calls == 0);
    assert(has2wifi.situation_calls == 1);
    assert(has2wifi.mine_calls == (has2wifi.situation_ok ? 1U : 0U));
    if (scenario == "deferred_approval") {
      delay(300);
      my["device_state"] = "open";
      DataChange();
      const auto opened = assert_pulse();
      assert_timing_log(1000, opened);
    }
  } else if (scenario == "reopen_ghost" || scenario == "reopen_survivor") {
    prepare("activate", "open");
    has2wifi.role = scenario == "reopen_ghost" ? "ghost" : "revival";
    card();
    assert(has2wifi.receive_calls == 1 && has2wifi.situation_calls == 0 && has2wifi.mine_calls == 0);
    if (scenario == "reopen_ghost") assert(assert_pulse() == 1300);
    else assert(on_count() == 0);
  } else if (scenario == "is_open_blocked") {
    prepare();
    has2wifi.already_open = 1;
    card();
    assert(on_count() == 0 && has2wifi.situation_calls == 0 && has2wifi.mine_calls == 0);
    assert(millis() - 1000 < 1500 && displayed_color == yellow);
  } else if (scenario == "tagger" || scenario == "admin_tagger" || scenario == "admin_ready" || scenario == "setting" || scenario == "invalid_tag") {
    prepare(scenario == "admin_ready" ? "ready" : scenario == "setting" ? "setting" : "activate",
            scenario.find("tagger") != std::string::npos ? "tagger" : "activate");
    card(scenario.find("admin_") == 0 ? "MMMM" : scenario == "invalid_tag" ? "BAD!" : "G1P1");
    assert_no_game_request();
    if (scenario.find("admin_") == 0) assert(assert_pulse() == 1000);
    else if (scenario == "setting") {
      assert(assert_pulse(SOLENOID_PULSE_MS) == 1000);
      gpio_events.clear();
      card();
      assert_pulse(SOLENOID_PULSE_MS);
      assert_no_game_request();
      assert(std::string((const char*)my["game_state"]) == "setting");
    } else assert(on_count() == 0);
  } else if (scenario == "timeout") {
    prepare();
    has2wifi.approve = false;
    card();
    delay(GHOST_OPEN_TIMEOUT_MS + 1);
    const auto before = millis();
    DataChange();
    assert(!ghost_open_pending && on_count() == 0 && millis() == before);
    unique_log("[GhostTiming] TIMEOUT waiting for open");
  } else assert(false && "Unknown test case");
  std::cout << "PASS " << scenario << '\n';
}
