// Host doubles only. Firmware declarations/functions are inserted by the runner.
#include <array>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>
using String = std::string;
using uint8_t = unsigned char;
using uint16_t = unsigned short;
constexpr int HIGH = 1, LOW = 0, RELAY_PIN = 1, EMCHECK_PIN = 2;
constexpr int NUMPIXELS_LINE = 30, DEFAULT_COLOR_BRIGHTNESS = 50, DEFAULT_LINE_BRIGHTNESS = 50;
enum GameState { setting, ready, activate };
unsigned long now = 0;
int relay = LOW, doorSensor = HIGH;
void digitalWrite(int, int value) { relay = value; }
int digitalRead(int pin) { return pin == RELAY_PIN ? relay : doorSensor; }
void delay(int) {}
unsigned long millis() { return now; }
struct Logger { template<class T> void print(T) {} template<class T> void println(T) {} } Serial;
struct Pixels {
    std::array<int, 3> color{};
    void lightColor(const int* value, int = 30) { color = {value[0], value[1], value[2]}; }
    void clear() { color = {}; } void show() {}
} pixels_line, pixels_switch, pixels_round;
struct Value {
    String text;
    Value& operator=(const char* s) { text = s; return *this; }
    operator const char*() const { return text.c_str(); }
    explicit operator int() const { return std::atoi(text.c_str()); }
};
std::map<String, Value> my, tag;
struct Wifi {
    std::vector<String> states, receives;
    void Send(String, String key, String value) { if (key == "device_state") states.push_back(value); }
    void Receive(String value) { receives.push_back(value); }
} has2wifi;
void Mp3PlayLargeFolder(uint8_t, uint16_t) {}
void CooltimeMp3() {}
void UpdateBrightness() {}

// SimpleTimer-compatible ordering: callbacks run before one-shot deletion;
// repeating callbacks deleted by themselves remain deleted. Reuse vacant IDs.
struct SimpleTimer {
    struct Entry { bool active = false, once = false; unsigned long delay = 0, prev = 0; void (*fn)() = nullptr; };
    std::array<Entry, 16> entries{};
    int create(unsigned long ms, void (*fn)(), bool once) {
        for (int i = 0; i < 16; ++i) if (!entries[i].active) {
            entries[i] = {true, once, ms, now, fn}; return i;
        }
        std::abort();
    }
    int setInterval(unsigned long ms, void (*fn)()) { return create(ms, fn, false); }
    int setTimeout(unsigned long ms, void (*fn)()) { return create(ms, fn, true); }
    bool isEnabled(int id) { return id >= 0 && id < 16 && entries[id].active; }
    void deleteTimer(int id) { if (id >= 0 && id < 16) entries[id] = {}; }
    void run() {
        std::array<int, 16> due{};
        for (int i = 0; i < 16; ++i) if (entries[i].active && now - entries[i].prev >= entries[i].delay) {
            entries[i].prev = now; due[i] = entries[i].once ? 2 : 1;
        }
        for (int i = 0; i < 16; ++i) if (due[i] && entries[i].active) {
            entries[i].fn(); if (due[i] == 2) deleteTimer(i);
        }
    }
} cooltime_timer, duct_close_timer, tagger_blink_timer;
// FIRMWARE_GLOBALS
// FIRMWARE_FUNCTIONS

void check(bool condition, const char* msg) {
    if (!condition) { std::cerr << "FAIL at " << now << " ms: " << msg << '\n'; std::exit(1); }
}
void advance(unsigned long ms) {
    auto end = now + ms;
    while (now < end) { ++now; cooltime_timer.run(); duct_close_timer.run(); tagger_blink_timer.run(); }
}
void openNormal() { DuctTag("G1P1"); check(relay == HIGH && !duct_available, "normal open"); }
void finished() { advance(7000); check(duct_available && relay == LOW, "cooldown completes"); }
void adminRepeat() {
    MmmmOpen(); check(mmmm_open && relay == HIGH, "admin may reopen");
    advance(4000); check(!mmmm_open && relay == LOW, "admin closes and clears flag");
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    String test = argv[1]; game_state = activate; cooltime_set = 5; cooltime_add = 0;
    my["device_name"] = "duct";
    if (test == "normal") {
        openNormal(); advance(3999); check(current_time == 0 && relay == HIGH, "no countdown before close");
        advance(1); check(relay == LOW, "normal closes at four seconds"); finished();
    } else if (test == "block_close_exit" || test == "block_exit_close") {
        openNormal(); advance(1000); EnterTaggerMode();
        has2wifi.states.clear();
        if (test == "block_exit_close") { advance(500); ExitTaggerMode(); }
        advance(5000); check(relay == LOW, "blockaded normal opening still closes");
        if (tagger_mode) {
            check(current_time == 0 && !duct_available, "cooldown frozen after close");
            check(has2wifi.states.empty(), "closing during blockade cannot overwrite server state");
            check(pixels_line.color == std::array<int, 3>{255, 0, 255}, "closing keeps blockade purple");
            ExitTaggerMode();
        }
        finished();
    } else if (test == "freeze_resume") {
        openNormal(); advance(6000); int elapsed = current_time; check(elapsed > 0, "countdown started");
        EnterTaggerMode(); advance(9000); check(current_time == elapsed, "blockade preserves elapsed");
        ExitTaggerMode(); advance(1000); check(current_time == elapsed + 1, "resumes remaining countdown"); finished();
    } else if (test == "admin_available") {
        adminRepeat(); check(duct_available && current_time == 0, "admin creates no cooldown"); adminRepeat();
    } else if (test == "admin_cooldown") {
        openNormal(); advance(6000); int elapsed = current_time;
        adminRepeat(); check(current_time == elapsed && !duct_available, "admin preserves cooldown"); finished();
    } else if (test == "admin_block_before_close" || test == "admin_block_after_close") {
        // Exercise both pre-existing cooldown and availability for this exit ordering.
        openNormal(); advance(6000); int elapsed = current_time;
        MmmmOpen(); advance(500); EnterTaggerMode();
        if (test == "admin_block_before_close") { advance(500); ExitTaggerMode(); }
        advance(5000); check(!mmmm_open && relay == LOW, "admin close survives blockade transition");
        if (tagger_mode) { check(current_time == elapsed, "admin cooldown remains frozen"); ExitTaggerMode(); }
        finished(); adminRepeat();
        MmmmOpen(); advance(500); EnterTaggerMode();
        if (test == "admin_block_before_close") ExitTaggerMode();
        advance(5000); check(!mmmm_open && relay == LOW, "available admin close clears flag");
        if (tagger_mode) ExitTaggerMode();
        check(duct_available, "available admin restores availability after blockade"); adminRepeat();
    } else if (test == "admin_inside_block") {
        EnterTaggerMode(); adminRepeat(); check(tagger_mode && duct_available, "admin preserves available blockade state");
        ExitTaggerMode(); openNormal(); advance(6000); EnterTaggerMode(); int elapsed = current_time;
        MmmmOpen(); advance(500); ExitTaggerMode(); advance(3500);
        check(!mmmm_open && current_time == elapsed, "admin opened during blockade preserves cooldown until close"); finished();
    } else if (test == "admin_early_back") {
        EnterTaggerMode(); MmmmOpen(); advance(500); ExitTaggerMode(); advance(3500);
        check(duct_available && !mmmm_open && relay == LOW, "admin early back restores available state");
        check(!has2wifi.states.empty() && has2wifi.states.back() == "activate", "server restored to activate after early back");
    } else if (test == "reset_pending_close") {
        openNormal(); advance(1000); SettingFunc(); advance(3000);
        check(duct_available && !cooltime_timer.isEnabled(cooltime_timer_id), "normal pending close does not restart cooldown after reset");
        check(relay == LOW, "reset pending opening still closes");
        game_state = activate; openNormal(); advance(6000); MmmmOpen(); advance(500); ReadyFunc(); advance(3500);
        check(duct_available && !cooltime_timer.isEnabled(cooltime_timer_id) && !mmmm_open,
              "admin pending close does not restore stale cooldown after reset");
    } else if (test == "normal_admin_override") {
        openNormal(); advance(2000); MmmmOpen(); advance(2001);
        check(relay == HIGH && mmmm_open && current_time == 0, "stale normal close cannot interrupt admin");
        advance(1999); check(relay == LOW && !mmmm_open, "override closes on its own schedule"); finished();
    } else if (test == "blocked_button") {
        EnterTaggerMode(); DuctOpen(true); check(relay == LOW, "blocked internal button cannot open");
        advance(4000); check(relay == LOW && switch_available, "button feedback ends locked");
        ExitTaggerMode(); openNormal(); advance(6000); EnterTaggerMode(); int elapsed = current_time;
        DuctOpen(true); advance(4000);
        check(relay == LOW && current_time == elapsed && !duct_available,
              "blocked button preserves existing cooldown"); ExitTaggerMode(); finished();
    } else if (test == "tagger_gate") {
        tag["role"] = "tagger"; my["tag_player"] = ""; uint8_t card[32] = {'G','1','T','1'};
        doorSensor = LOW; CardChecking(card); check(!tagger_mode, "closed door cannot be blockaded");
        doorSensor = HIGH; CardChecking(card); check(tagger_mode, "tagger with open door works without recent player");
        check(has2wifi.receives.size() == 2, "only current card looked up");
    } else return 2;
    std::cout << "PASS " << test << '\n';
}
