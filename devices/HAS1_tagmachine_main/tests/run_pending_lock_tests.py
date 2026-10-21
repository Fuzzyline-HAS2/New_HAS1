#!/usr/bin/env python3
"""Exercise the production lock/unlock state machine against a modelled server.

Regression for the stale pending lock (updated_tagmachine_main 6c0d8d8, ported here):
in normal (hard) mode, lock -> unlock -> lock -> unlock left the door locked, because
the lock completion only refreshed `my` (ReceiveMine) and never ran DataChanged(), so
DataChanged's `cur` stayed "activate"; the next unlock's completion re-check then saw
"activate -> lock" late, queued it (loginDone), and ApplyPendingDeviceState re-applied
that stale lock after the unlock finished.

Production code under test (extracted verbatim from the sketch): DataChanged,
ApplyDeviceState, Queue/Clear/ApplyPendingDeviceState, TaggerOverrideCheck,
ReturnNormalState, CancelTagProgress, DoorOpen, PlayerLockTimerFunc,
PlayerUnlockTimerFunc. Everything else (server, HTTP, timers, LEDs, Beetle OTA,
BLE) is a double. The server model mirrors the store 1 ESP route: a device's own
Send does NOT raise shift_machine, so has2wifi.Loop(DataChanged) never re-reads
`my` on its own after a self-sent transition.
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


production = "\n".join([
    function("wifi.ino", "QueuePendingDeviceState"),
    function("wifi.ino", "ClearPendingDeviceState"),
    function("wifi.ino", "ApplyPendingDeviceState"),
    function("wifi.ino", "ApplyDeviceState"),
    function("wifi.ino", "TaggerOverrideCheck"),
    function("wifi.ino", "DataChanged"),
    function("timer.ino", "ReturnNormalState"),
    function("timer.ino", "CancelTagProgress"),
    function("Game_system.ino", "DoorOpen"),
    function("timer.ino", "PlayerLockTimerFunc"),
    function("timer.ino", "PlayerUnlockTimerFunc"),
])

source = r'''
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>
using String = std::string;
constexpr int LOW = 0, HIGH = 1, RELAY_PIN = 21;
enum { BLACK, WHITE, RED, GREEN, YELLOW, PURPLE, BLUE };
enum { ROUND, LINE };
int NumPixels[] = {24, 30};
static inline int GaugeMap(int cnt, int total, int pixels) { return total <= 0 ? pixels : cnt * pixels / total; }

// ---- ArduinoJson double: string-valued document with the few accessors the sketch uses ----
struct Doc;
struct Proxy {
  std::map<std::string, std::string> *m; std::string k;
  operator const char *() const { auto it = m->find(k); return it == m->end() ? "" : it->second.c_str(); }
  operator int() const { return std::atoi(static_cast<const char *>(*this)); }
  Proxy &operator=(const char *v) { (*m)[k] = v; return *this; }
  Proxy &operator=(const std::string &v) { (*m)[k] = v; return *this; }
  template <class T> bool is() const { return m->count(k) > 0; }
  template <class T> const char *as() const { return static_cast<const char *>(*this); }
};
struct JsonDocument {
  std::map<std::string, std::string> m;
  Proxy operator[](const char *k) { return Proxy{&m, k}; }
};
JsonDocument my;

// ---- observable state ----
std::vector<std::string> events;
int relay = LOW, last_color = -1;
bool send_bumps_shift = false;   // store 1 ESP route: a device's own Send leaves shift_machine alone
struct Log {
  template <class T> void println(const T &) {}
  template <class... T> void printf(const char *, T...) {}
} Serial;
void digitalWrite(int pin, int v) { assert(pin == RELAY_PIN); relay = v; events.push_back(v == HIGH ? "relay_high" : "relay_low"); }
void delay(unsigned long) {}
void AllNeoOn(int c) { last_color = c; events.push_back(c == GREEN ? "neo_green" : c == YELLOW ? "neo_yellow" : "neo_other"); }
void RoundNeoEffect(int) {} void RoundNeoEffectDown(int) {} void RoundNeoToggle(int, int) {}
void LineNeoUp(int, int, int) {} void LineNeoDown(int, int, int) {}
void SubSerialFlush() {} void MainSerialFlush() {}

// ---- server + HTTP double ----
struct Wifi {
  std::map<std::string, std::string> server;  // the device row as the server holds it
  bool shift = false;
  void Send(String, const char *column, const char *value) {
    server[column] = value; events.push_back(std::string("send:") + column + "=" + value);
    if (send_bumps_shift) shift = true;
  }
  void Send(String d, const char *column, const String &value) { Send(d, column, value.c_str()); }
  void ReceiveMine() { for (auto &kv : server) my.m[kv.first] = kv.second; events.push_back("receive"); }
  void Loop(void (*f)()) { if (!shift) return; shift = false; ReceiveMine(); f(); }
  void dashboard(const char *column, const char *value) { server[column] = value; shift = true; }
} has2wifi;

// ---- timers / pointers / misc globals the production functions touch ----
struct Timer { int next = 1; int setInterval(int, void (*)()) { return next++; } void deleteTimer(int) {} } GameTimer, SubSerialTimer, WifiTimer, DebuffTimer;
int gameTimerId = 0, subSerialTimerId = 0, wifiTimerId = 0, debuffTimerId = 0;
bool SubSerialTimerStart = false, loginDone = false;
int gameTimerCnt = 0, playerLockTime = 0, playerUnlockTime = 0;
String strCurState = "";
String pendingDeviceState = "";
bool pendingDeviceStateApply = false;
void (*ptrCurrentMode)() = nullptr; void (*ptrRfidMode)(char) = nullptr; void (*ptrRfidFail)() = nullptr;
void (*ptrRfidMain)() = nullptr; void (*ptrRfidSub)() = nullptr;
void WaitFunc() {} void WaitRfid(char) {} void Login(char) {} void NewbieLogin(char) {}
void CommnunicationBeetle() {} void CommnunicationMainBeetle() {} void WhichTagged() {}
void WifiIntervalFunc() {}
void DebuffTimerFunc() {}
bool BeetleOtaActive() { return false; }
bool IsTtgoOnlyOtaCommand(const String &) { return false; }
bool IsAllBoardOtaCommand(const String &) { return false; }
bool IsExpectedAllBoardOtaCommand(const String &) { return true; }
void BeginTtgoOnlyOta() {} void CancelTtgoOnlyOta() {} void BeginBeetleOtaSequence() {}
void NoteBeetleOtaCommandChanged() {} void NoteBeetleOtaRuntimeUnsafe() {}
namespace Has1BleBeacon { void setDeviceName(const char *) {} }
void UpdateBrightness() {} void GameSetting() {} void NewbieModeSetting() {}
void SettingFunc() { AllNeoOn(WHITE); } void ReadyFunc() { AllNeoOn(RED); }
void ActivateFunc() { AllNeoOn(YELLOW); digitalWrite(RELAY_PIN, LOW); }
void GhostDoorOpen() {}

// production prototypes (Arduino generates these for the sketch)
void QueuePendingDeviceState(String); void ClearPendingDeviceState(); void ApplyPendingDeviceState();
void ApplyDeviceState(String); bool TaggerOverrideCheck(); void DataChanged();
void ReturnNormalState(); void CancelTagProgress(); void DoorOpen();
void PlayerLockTimerFunc(); void PlayerUnlockTimerFunc();

// PRODUCTION

// ---- scenario helpers ----
void expect_state(const char *server_state, const char *cur_state, bool pending) {
  assert(has2wifi.server["device_state"] == server_state);
  assert(strCurState == cur_state);
  assert(pendingDeviceStateApply == pending);
}
void tagLock()   { loginDone = true; gameTimerCnt = 0; PlayerLockTimerFunc();   ApplyPendingDeviceState(); }
void tagUnlock() { loginDone = true; gameTimerCnt = 0; PlayerUnlockTimerFunc(); ApplyPendingDeviceState(); }
void boot(bool bumps) {
  events.clear(); relay = LOW; last_color = -1; send_bumps_shift = bumps;
  has2wifi = Wifi(); my = JsonDocument(); loginDone = false; gameTimerCnt = 0;
  strCurState = ""; pendingDeviceState = ""; pendingDeviceStateApply = false;
  has2wifi.server = {{"device_name", "AT"}, {"game_state", "activate"}, {"device_state", "activate"}, {"mode", "normal"}};
  has2wifi.ReceiveMine(); DataChanged();        // first poll of a running game
  // DataChanged keeps `cur` in a function static: re-sync it to this scenario's start state.
  expect_state("activate", "activate", false);
}
int main(int argc, char **argv) {
  assert(argc == 2);
  std::string scenario = argv[1];
  if (scenario == "cycle" || scenario == "cycle_shift") {
    // The reported symptom: lock -> unlock -> lock -> unlock must end unlocked every time,
    // with the server, strCurState and the LEDs all agreeing on "activate".
    boot(scenario == "cycle_shift");
    for (int i = 0; i < 3; ++i) {
      tagLock();
      expect_state("lock", "lock", false);
      assert(last_color == GREEN && relay == LOW && !loginDone);
      tagUnlock();
      expect_state("activate", "activate", false);
      assert(last_color == YELLOW && relay == LOW && !loginDone);
      // A later idle poll must not resurrect the lock either.
      has2wifi.Loop(DataChanged); ApplyPendingDeviceState();
      expect_state("activate", "activate", false);
      assert(last_color == YELLOW);
    }
  } else if (scenario == "lock_syncs_cur") {
    // Second half of the fix: after a self-sent lock, DataChanged's cur must already be
    // "lock", so the unlock's completion re-check queues nothing while loginDone is set.
    boot(false);
    tagLock();
    loginDone = true;                 // the next unlock is in progress
    has2wifi.ReceiveMine(); DataChanged();
    assert(!pendingDeviceStateApply);
    loginDone = false;
  } else if (scenario == "stale_queue_dropped") {
    // First half of the fix: whatever got queued during the unlock is stale once the
    // unlock succeeds (the firmware just sent open/activate itself) and must be dropped.
    boot(false);
    tagLock();
    loginDone = true; gameTimerCnt = 0;
    QueuePendingDeviceState("lock");  // what the pre-fix re-check used to leave behind
    PlayerUnlockTimerFunc();
    assert(!pendingDeviceStateApply);
    ApplyPendingDeviceState();
    expect_state("activate", "activate", false);
    assert(last_color == YELLOW);
  } else if (scenario == "dashboard_unlock_mid_unlock") {
    // The staff unlocks from the dashboard while a survivor's unlock timer runs: the
    // completion re-check sees "activate" and must cancel cleanly, still ending unlocked.
    boot(false);
    tagLock();
    loginDone = true; gameTimerCnt = 0;
    has2wifi.dashboard("device_state", "activate");
    PlayerUnlockTimerFunc(); ApplyPendingDeviceState();
    expect_state("activate", "activate", false);
    assert(!loginDone && last_color == YELLOW);
  } else if (scenario == "dashboard_lock_mid_lock") {
    // Dashboard locks while the survivor's lock timer runs: re-check sees "lock",
    // cancels, and the queued lock is applied by CancelTagProgress (door locked, green).
    boot(false);
    loginDone = true; gameTimerCnt = 0;
    has2wifi.dashboard("device_state", "lock");
    PlayerLockTimerFunc(); ApplyPendingDeviceState();
    expect_state("lock", "lock", false);
    assert(!loginDone && last_color == GREEN && relay == LOW);
  } else if (scenario == "tagger_override_during_unlock") {
    // Server flips to "tagger" during the door-open effect: DoorOpen must apply tagger
    // and skip the activate send; nothing stale may be queued or applied afterwards.
    boot(false);
    tagLock();
    loginDone = true; gameTimerCnt = 0;
    has2wifi.ReceiveMine(); DataChanged();    // completion re-check (nothing queued: cur is lock)
    assert(!pendingDeviceStateApply);
    has2wifi.dashboard("device_state", "tagger");
    PlayerUnlockTimerFunc(); ApplyPendingDeviceState();
    assert(has2wifi.server["device_state"] == "tagger");
    assert(strCurState == "tagger" && relay == HIGH && !pendingDeviceStateApply);
  } else {
    std::cerr << "unknown scenario " << scenario << "\n";
    return 1;
  }
  std::cout << "PASS " << scenario << "\n";
  return 0;
}
'''.replace("// PRODUCTION", production)

with tempfile.TemporaryDirectory(prefix="tagmachine-pending-lock-") as temp:
    src, exe = Path(temp) / "test.cpp", Path(temp) / "test"
    src.write_text(source, encoding="utf-8")
    subprocess.run(["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(src), "-o", str(exe)], check=True)
    for scenario in ("cycle", "cycle_shift", "lock_syncs_cur", "stale_queue_dropped",
                     "dashboard_unlock_mid_unlock", "dashboard_lock_mid_lock",
                     "tagger_override_during_unlock"):
        # Each scenario in its own process: DataChanged keeps `cur` in a function static.
        subprocess.run([str(exe), scenario], check=True)
