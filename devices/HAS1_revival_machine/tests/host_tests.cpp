// Deterministic host fakes; the behavior under test comes from production .ino files.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <limits>
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
bool revival_approval_pending = false;
bool revival_approval_poll_due = false, revival_approval_polled_this_loop = false;
unsigned long revival_approval_started_ms = 0, revival_approval_last_poll_ms = 0;
unsigned long revival_approval_last_admin_poll_ms = 0;
String revival_request_device_state;
bool gameplay_tag_latched = false, gameplay_tag_missing = false;
String gameplay_tag_user;
unsigned long gameplay_tag_missing_since_ms = 0;
unsigned gameplay_tag_miss_count = 0;
bool rfid_tag = false;
int rfid_timer_id = 0, nsec_tag_timer_id = 0, wifi_timer_id = 0;
int nsec_tag_num = 0;
bool nsec_tag_bool = false;
struct FakeTimer {
  struct Task { int id; unsigned long last, period; void (*callback)(); bool repeat, active; };
  std::vector<Task> tasks;
  int next_id = 0;
  unsigned runs = 0;
  int setInterval(unsigned long ms, void (*callback)()) { return add(ms, callback, true); }
  int setTimeout(unsigned long ms, void (*callback)()) { return add(ms, callback, false); }
  int add(unsigned long ms, void (*callback)(), bool repeat) {
    tasks.push_back({++next_id, millis(), ms, callback, repeat, true});
    return next_id;
  }
  void deleteTimer(int id) { for (auto& task : tasks) if (task.id == id) task.active = false; }
  void restartTimer(int id) { for (auto& task : tasks) if (task.id == id) task.last = millis(); }
  void run() {
    ++runs;
    std::vector<int> due;
    for (const auto& task : tasks)
      if (task.active && millis() - task.last >= task.period) due.push_back(task.id);
    for (const auto id : due) {
      void (*callback)() = nullptr;
      for (auto& task : tasks) if (task.id == id && task.active) {
        callback = task.callback; task.last += task.period;
        if (!task.repeat) task.active = false;
        break;
      }
      if (callback) callback();
    }
  }
} rfid_timer, nsec_tag_timer, wifi_timer;
int white[3], red[3], yellow[3], blue[3], purple[3];
int* displayed_color = nullptr;
void NeoNo() {}
void (*NeoFunc)() = NeoNo;
void NeopixelSet(int* color) { displayed_color = color; delay(10); }
void SetWifiPollInterval(unsigned long);
void RfidLoop();
void CardChecking(uint8_t data[32]);
void AdminCardPollReady();
void AdminCardPollPending();
void RfidTagTimerFunc();
void WifiTimerFunc();
void NsecTagTimerFailFunc();
void NsecTagTimerSuccessFunc();
void BeginRevivalApproval(unsigned long);
void EndRevivalApproval(const char*, bool preserveUser = false);
void UpdateRevivalApprovalState();
void PollRevivalApproval();
void ObserveGameplayTag(bool);
void BleAdvertiserMaintain() {}
void TelnetRun() {}
unsigned normal_reader_calls = 0, admin_reader_calls = 0;
bool reader_present = true;
std::string reader_tag = "G1P1";
bool fake_read(uint8_t data[32]) {
  if (!reader_present) return false;
  std::memset(data, 0, 32);
  std::memcpy(data, reader_tag.c_str(), 4);
  return true;
}
bool DetectTag(uint8_t data[32]) { ++normal_reader_calls; return fake_read(data); }
bool DetectAndRead(uint8_t data[32]) { ++admin_reader_calls; return fake_read(data); }
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
  unsigned receive_calls = 0, situation_calls = 0, mine_calls = 0, send_calls = 0, loop_calls = 0;
  std::vector<std::string> received_tags, situation_tags, marked_tags;
  unsigned long role_delay_ms = 300, situation_delay_ms = 200, approval_delay_ms = 400;
  bool situation_ok = true, approve = true;
  std::string role = "ghost";
  int already_open = 0;
  void Receive(const String& user) {
    ++receive_calls; received_tags.push_back(user);
    fake_ms += role_delay_ms;
    tag["role"] = role.c_str();
    tag["is_open"] = already_open;
  }
  bool Situation(const String& user, const String&) {
    ++situation_calls; situation_tags.push_back(user);
    fake_ms += situation_delay_ms;
    return situation_ok;
  }
  void ReceiveMine() {
    ++mine_calls;
    fake_ms += approval_delay_ms;
    if (approve) my["device_state"] = "open";
  }
  void Send(const String& user, const String& field, const String& value) {
    assert(field == "is_open" && value == "1");
    ++send_calls; marked_tags.push_back(user);
  }
  void Loop(void (*)()) { ++loop_calls; }
} has2wifi;

#define _HAS1_REVIVAL_MACHINE_H_
#include "approval.ino"
#include "game_state.ino"
#include "timer.ino"
#include "sensor_under_test.inc"
#include "loop_under_test.inc"

void prepare(const char* game_state = "activate", const char* device_state = "activate") {
  TimerInit();
  my["device_name"] = "revival_machine_original";
  my["game_state"] = game_state;
  my["device_state"] = "activate";
  my["brightness"] = 50;
  DataChange();
  my["device_state"] = device_state;
  gpio_events.clear();
  Serial.lines.clear();
  fake_ms = 1000;
  for (auto& task : wifi_timer.tasks) task.last = fake_ms;
}
void card(const char* name = "G1P1") {
  uint8_t data[32] = {};
  std::memcpy(data, name, 4);
  CardChecking(data);
}
void scan(bool present = true, const char* name = "G1P1") {
  reader_present = present;
  reader_tag = name;
  RfidTagTimerFunc();
  RfidLoop();
}
void remove_tag() {
  scan(false);
  delay(RFID_REARM_ABSENT_MS);
  scan(false);
}
unsigned long wifi_period() {
  for (const auto& task : wifi_timer.tasks)
    if (task.id == wifi_timer_id && task.active) return task.period;
  assert(false && "wifi_timer not armed");
  return 0;
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
      has2wifi.approve = true;
      TimerRun();
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
    if (scenario == "admin_ready") { reader_tag = "MMMM"; AdminCardPollReady(); }
    else card(scenario.find("admin_") == 0 ? "MMMM" : scenario == "invalid_tag" ? "BAD!" : "G1P1");
    assert_no_game_request();
    if (scenario.find("admin_") == 0) assert(assert_pulse() == 1000);
    else if (scenario == "setting") {
      assert(assert_pulse(SOLENOID_PULSE_MS) == 1000);
      gpio_events.clear();
      scan();
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
    TimerRun();
    assert(!ghost_open_pending && !revival_approval_pending && on_count() == 0 && millis() == before);
    unique_log("[GhostTiming] TIMEOUT waiting for open");
  } else if (scenario == "held_pending") {
    prepare();
    has2wifi.approve = false;
    card();
    assert(revival_approval_pending);
    const auto started = revival_approval_started_ms;
    const auto ghost_started = ghost_tag_start_ms;
    const auto polls = ghost_poll_count;
    const String user = last_open_tag_user;
    card(); card("G2P2"); card();
    assert(has2wifi.receive_calls == 1 && has2wifi.situation_calls == 1);
    assert(revival_approval_started_ms == started && ghost_tag_start_ms == ghost_started);
    assert(ghost_poll_count == polls && last_open_tag_user == user && user == "G1P1");
    assert(on_count() == 0);
  } else if (scenario == "non_ghost_then_ghost") {
    prepare(); has2wifi.approve = false; has2wifi.role = "revival";
    card();
    assert(!revival_approval_pending && !ghost_open_pending && last_open_tag_user.empty());
    card();
    assert(has2wifi.receive_calls == 1 && has2wifi.situation_calls == 1);
    has2wifi.role = "ghost";
    card("G2P2");
    assert(revival_approval_pending && ghost_open_pending && last_open_tag_user == "G2P2");
    assert(has2wifi.receive_calls == 2 && has2wifi.situation_calls == 2 && on_count() == 0);
  } else if (scenario == "priority_scheduler") {
    prepare(); has2wifi.approve = false; card();
    const auto polls = has2wifi.mine_calls;
    const auto ordinary_runs = wifi_timer.runs;
    for (unsigned i = 1; i <= 8; ++i) {
      delay(REVIVAL_APPROVAL_POLL_MS);
      loop();
      assert(has2wifi.mine_calls == polls + i);
      assert(normal_reader_calls == 0 && admin_reader_calls == 0);
      assert(has2wifi.receive_calls == 1 && has2wifi.situation_calls == 1);
      assert(has2wifi.loop_calls == 0 && wifi_timer.runs == ordinary_runs);
    }
  } else if (scenario == "pending_admin" || scenario == "pending_admin_rate") {
    prepare(); has2wifi.approve = false; card();
    const auto user = last_open_tag_user;
    delay(REVIVAL_ADMIN_POLL_MS + 1);
    reader_tag = scenario == "pending_admin" ? "MMMM" : "G2P2";
    TimerRun();  // Approval is due and must be processed before the admin probe.
    assert(admin_reader_calls == 0);
    loop();      // The next iteration has no due approval and permits the sparse probe.
    assert(admin_reader_calls == 1 && normal_reader_calls == 0);
    assert(has2wifi.receive_calls == 1 && has2wifi.situation_calls == 1);
    assert(revival_approval_pending && last_open_tag_user == user);
    if (scenario == "pending_admin") assert_pulse();
    else {
      RfidLoop(); delay(REVIVAL_ADMIN_POLL_MS - 1); RfidLoop();
      assert(admin_reader_calls == 1 && on_count() == 0);
    }
  } else if (scenario == "failure_held" || scenario == "timeout_held") {
    prepare(); has2wifi.approve = false;
    has2wifi.situation_ok = scenario != "failure_held";
    card();
    if (scenario == "timeout_held") { delay(REVIVAL_APPROVAL_TIMEOUT_MS + 1); TimerRun(); }
    assert(!revival_approval_pending && !ghost_open_pending && last_open_tag_user == "G1P1");
    card(); card();
    assert(has2wifi.receive_calls == 1 && has2wifi.situation_calls == 1);
    assert(on_count() == 0);
  } else if (scenario == "removal_rearms" || scenario == "miss_does_not_rearm" || scenario == "different_tag") {
    prepare(); has2wifi.situation_ok = false; card();
    assert(gameplay_tag_latched);
    if (scenario == "different_tag") card("G2P2");
    else {
      scan(false);
      delay(RFID_REARM_ABSENT_MS - 1);
      if (scenario == "removal_rearms") {
        scan(false); assert(gameplay_tag_latched);
        delay(1); scan(false);
        assert(!gameplay_tag_latched);
      } else {
        delay(1); assert(gameplay_tag_latched);
      }
      scan(true);
    }
    const unsigned expected = scenario == "miss_does_not_rearm" ? 1 : 2;
    assert(has2wifi.receive_calls == expected && has2wifi.situation_calls == expected);
    assert(!revival_approval_pending && on_count() == 0);
  } else if (scenario == "reset_ready" || scenario == "reset_setting" || scenario == "reset_tagger") {
    prepare(); has2wifi.approve = false; card();
    assert(revival_approval_pending);
    if (scenario == "reset_tagger") my["device_state"] = "tagger";
    else my["game_state"] = scenario == "reset_ready" ? "ready" : "setting";
    DataChange();
    assert(!revival_approval_pending && !ghost_open_pending && last_open_tag_user.empty());
    assert(on_count() == 0 && has2wifi.send_calls == 0);
  } else if (scenario == "reopen_after_removal") {
    prepare(); card(); assert_pulse();
    const auto received = has2wifi.receive_calls;
    card();
    assert(on_count() == 1 && has2wifi.receive_calls == received);
    remove_tag(); gpio_events.clear(); scan(true);
    assert_pulse();
    assert(has2wifi.receive_calls == received + 1 && has2wifi.situation_calls == 1);
    assert(has2wifi.marked_tags.size() == 1 && has2wifi.marked_tags.front() == "G1P1");
  } else if (scenario == "normal_poll_resume") {
    prepare(); has2wifi.approve = false; card();
    delay(REVIVAL_APPROVAL_TIMEOUT_MS + 1);
    TimerRun();
    assert(!revival_approval_pending && has2wifi.loop_calls == 0);
    for (unsigned i = 0; i < 20; ++i) TimerRun();
    assert(has2wifi.loop_calls == 0);
    delay(WIFI_POLL_INTERVAL_ACTIVATE_MS);
    TimerRun();
    assert(has2wifi.loop_calls == 1);
    for (unsigned i = 0; i < 20; ++i) TimerRun();
    assert(has2wifi.loop_calls == 1);
  } else if (scenario == "late_approval_identity" || scenario == "late_failure_identity" || scenario == "cancelled_late_approval") {
    prepare(); has2wifi.approve = false;
    has2wifi.situation_ok = scenario != "late_failure_identity";
    card();
    if (scenario != "late_failure_identity") { delay(REVIVAL_APPROVAL_TIMEOUT_MS + 1); TimerRun(); }
    assert(!revival_approval_pending && !ghost_open_pending && last_open_tag_user == "G1P1");
    if (scenario == "cancelled_late_approval") {
      my["game_state"] = "ready"; DataChange();
      assert(last_open_tag_user.empty());
    }
    my["device_state"] = "open"; DataChange();
    assert_pulse();
    if (scenario == "cancelled_late_approval") assert(has2wifi.send_calls == 0 && has2wifi.marked_tags.empty());
    else assert(has2wifi.marked_tags.size() == 1 && has2wifi.marked_tags.front() == "G1P1");
  } else if (scenario == "timeout_clock_wrap") {
    prepare(); fake_ms = std::numeric_limits<unsigned long>::max() - 100;
    has2wifi.approve = false; card();
    assert(revival_approval_pending);
    delay(REVIVAL_APPROVAL_TIMEOUT_MS + 1);
    TimerRun();
    assert(!revival_approval_pending && on_count() == 0 && last_open_tag_user == "G1P1");
  } else if (scenario == "mode_ready_to_activate_device_static") {
    // 서버가 game_state만 ready -> activate 로 바꾸고 device_state는 내내 "activate"인 경우.
    // 예전 코드는 device_state 전이에만 노란색/activate 폴링을 걸어 ready의 빨간색에 머물렀다.
    prepare("ready", "activate");
    assert(displayed_color == red && !activate_bool);
    my["game_state"] = "activate"; DataChange();
    assert(displayed_color == yellow && activate_bool);
    assert(wifi_period() == WIFI_POLL_INTERVAL_ACTIVATE_MS);
    assert(on_count() == 0 && has2wifi.send_calls == 0);
  } else if (scenario == "mode_game_change_keeps_relay_quiet") {
    // 열린 뒤 game_state가 바뀌어도(라운드 종료) 솔레노이드와 is_open 기록은 다시 일어나지 않는다.
    prepare(); card(); assert_pulse();
    assert(has2wifi.send_calls == 1);
    my["game_state"] = "ready"; DataChange();
    assert(on_count() == 1 && has2wifi.send_calls == 1);
    assert(displayed_color == red && !activate_bool);
    my["game_state"] = "setting"; DataChange();
    assert(on_count() == 1 && has2wifi.send_calls == 1 && displayed_color == white);
  } else if (scenario == "mode_ready_ignores_device_rearm") {
    // ready 중에 서버가 device_state를 open -> activate 로 되돌려도 ready의 빨간색을 덧칠하지 않는다.
    prepare("ready", "activate");
    my["device_state"] = "open"; DataChange();
    gpio_events.clear();
    my["device_state"] = "activate"; DataChange();
    assert(displayed_color == red && !activate_bool && on_count() == 0);
    assert(wifi_period() == WIFI_POLL_INTERVAL_DEFAULT_MS);
    // 이후 game_state가 activate로 바뀌면 그때 조합이 (activate, activate)가 되어 노란색이 된다.
    my["game_state"] = "activate"; DataChange();
    assert(displayed_color == yellow && activate_bool);
    assert(wifi_period() == WIFI_POLL_INTERVAL_ACTIVATE_MS);
  } else assert(false && "Unknown test case");
  std::cout << "PASS " << scenario << '\n';
}
