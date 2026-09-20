// Host doubles only. Firmware declarations/functions are inserted by the runner.
#include <array>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <limits>
using String = std::string;
using uint8_t = unsigned char;
using uint16_t = unsigned short;
#include "audio_queue.h"
#include "mp3_durations.h"
constexpr int HIGH = 1, LOW = 0, RELAY_PIN = 1, EMCHECK_PIN = 2, SW_PIN = 15;
constexpr int NUMPIXELS_LINE = 30, DEFAULT_COLOR_BRIGHTNESS = 50, DEFAULT_LINE_BRIGHTNESS = 50;
enum GameState { setting, ready, activate };
unsigned long now = 0;
int relay = LOW, doorSensor = HIGH, switchInput = HIGH;
std::vector<String> audioEvents;
void digitalWrite(int, int value) { relay = value; }
int digitalRead(int pin) { return pin == RELAY_PIN ? relay : pin == SW_PIN ? switchInput : doorSensor; }
// Record blocking audio delays without advancing the timer scheduler.
void delay(unsigned long ms) { audioEvents.push_back("delay:" + std::to_string(ms)); }
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
std::map<String, Value> my, tag, shift_machine;
struct Wifi {
    std::vector<String> states, receives;
    void Send(String, String key, String value) { if (key == "device_state") states.push_back(value); }
    void Receive(String value) { receives.push_back(value); }
} has2wifi;
int cooldownAnnouncements = 0, blockedAnnouncements = 0;
std::vector<unsigned long> audioStartTimes;
void recordTrack(uint8_t folder, uint16_t file) {
    if (folder == 4 && file == 2) ++blockedAnnouncements;
    if (folder == 1 && file == 3) ++cooldownAnnouncements;
    audioEvents.push_back("play:" + std::to_string(folder) + ":" + std::to_string(file));
    audioStartTimes.push_back(now);
}
#ifdef ACTUAL_AUDIO
struct Player {
    int events = 0;
    bool available() { return events > 0; }
    int readType() { return 0; }
    int read() { --events; return 0; }
    void volume(int) {}
    void playLargeFolder(uint8_t folder, uint16_t file) { recordTrack(folder, file); }
} myDFPlayer;
#else
void Mp3QueuePhrase(Mp3Phrase phrase) {
    for (uint8_t i = 0; i < phrase.count; ++i) recordTrack(phrase.tracks[i].folder, phrase.tracks[i].file);
}
// DOMAIN_AUDIO_FACTORY
#endif
void RfidLoop() {}
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
#ifdef ACTUAL_AUDIO
// ACTUAL_AUDIO_FUNCTIONS
#endif
// FIRMWARE_FUNCTIONS

void check(bool condition, const char* msg) {
    if (!condition) { std::cerr << "FAIL at " << now << " ms: " << msg << '\n'; std::exit(1); }
}
void advance(unsigned long ms) {
    auto end = now + ms;
    while (now < end) {
        ++now; cooltime_timer.run(); duct_close_timer.run(); tagger_blink_timer.run();
#ifdef ACTUAL_AUDIO
        Mp3Run();
#endif
    }
}
void openNormal() { DuctTag("G1P1"); check(relay == HIGH && !duct_available, "normal open"); }
// 내부 스위치 한 번 눌렀다 떼기 (누른 상태로 남기지 않는다).
void pressSwitch() {
    switchInput = HIGH; ActivateFunc();
    switchInput = LOW;  ActivateFunc();
    switchInput = HIGH; ActivateFunc();
}
void finished() { advance(7000); check(duct_available && relay == LOW, "cooldown completes"); }
void adminRepeat() {
    MmmmOpen(); check(mmmm_open && relay == HIGH, "admin may reopen");
    advance(4000); check(!mmmm_open && relay == LOW, "admin closes and clears flag");
}
std::vector<String> blockadeAudioEvents(int seconds) {
    return {
        "play:4:2", "play:3:" + std::to_string(seconds), "play:1:5"
    };
}
void expectBlockadeAudio(int seconds) {
    audioEvents.clear(); tag["role"] = "player";
    uint8_t card[32] = {'G', '1', 'P', '1'};
    CardChecking(card);
    check(audioEvents == blockadeAudioEvents(seconds), "blocked outside player tag announces remaining blockade seconds");
    check(tagger_mode && relay == LOW, "remaining-time announcement keeps blockade locked");
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
    } else if (test == "cooldown_button_feedback") {
        openNormal(); advance(6000); int elapsed = current_time;
        DuctTag("G1P2"); check(cooldownAnnouncements == 1, "outside tag announces cooldown");
        switchInput = LOW; ActivateFunc();
        check(cooldownAnnouncements == 2, "cooldown button uses same announcement as outside tag");
        check(relay == LOW && current_time == elapsed && !duct_available,
              "cooldown button does not open or reset countdown");
        for (int i = 0; i < 100; ++i) { advance(10); ActivateFunc(); }
        check(cooldownAnnouncements == 2, "held button does not restart announcement");
        check(current_time == elapsed + 1 && relay == LOW, "cooldown continues while button held");
        switchInput = HIGH; ActivateFunc(); switchInput = LOW; ActivateFunc();
        check(cooldownAnnouncements == 3, "release and repress announces again");
        check(current_time == elapsed + 1 && relay == LOW, "repress preserves remaining cooldown");
    } else if (test == "blockade_button_feedback") {
        openNormal(); advance(6000); EnterTaggerMode(); int elapsed = current_time;
        audioEvents.clear();
        switchInput = LOW; ActivateFunc();
        check(blockedAnnouncements == 1 && cooldownAnnouncements == 0, "blockade feedback takes priority over cooldown");
        check(audioEvents == blockadeAudioEvents(30), "internal button uses same blockade intro and remaining time as external tag");
        check(switch_available && !duct_close_timer.isEnabled(duct_close_timer_id),
              "blockade announcement does not disable button or schedule delayed reenable");
        ActivateFunc();
        check(blockedAnnouncements == 1, "holding after first announcement does not repeat");
        audioEvents.clear();
        switchInput = HIGH; ActivateFunc(); switchInput = LOW; ActivateFunc();
        check(blockedAnnouncements == 2 && audioEvents == blockadeAudioEvents(30),
              "immediate release and repress repeats announcement without four second wait");
        for (int i = 0; i < 500; ++i) { advance(10); ActivateFunc(); }
        check(blockedAnnouncements == 2 && cooldownAnnouncements == 0, "held blockaded button does not repeat feedback");
        check(relay == LOW && current_time == elapsed, "blockaded button stays closed and frozen");
        audioEvents.clear();
        switchInput = HIGH; ActivateFunc(); switchInput = LOW; ActivateFunc();
        check(blockedAnnouncements == 3 && cooldownAnnouncements == 0, "blockaded repress repeats correct feedback");
        check(audioEvents == blockadeAudioEvents(25), "blockaded repress announces updated remaining time");
        check(relay == LOW && tagger_mode, "repeated blockade announcement never opens or releases door");
    } else if (test == "switch_counts") {
        my["cool_time"] = "5"; my["cool_time_add"] = "5";
        ReadyFunc(); ActivateRunOnce();
        check(cooltime == 5 && use_duct_num == 0, "activate seeds the first cooldown before any opening");
        pressSwitch();
        check(relay == HIGH && use_duct_num == 1 && cooltime == 5, "inside switch counts as one use");
        advance(4000);
        check(relay == LOW && cooltime_timer.isEnabled(cooltime_timer_id), "switch opening arms the cooldown");
        advance(1000); check(!duct_available && current_time == 1, "switch opening holds a real cooldown");
        advance(5000); check(duct_available && current_time == 0, "switch cooldown releases after cool_time");
        pressSwitch(); check(use_duct_num == 2 && cooltime == 5, "second switch use stays on the first ladder step");
        advance(10000); check(duct_available, "second switch cooldown completes");
        pressSwitch(); check(use_duct_num == 3 && cooltime == 10, "third use escalates the cooldown from switch openings");
        advance(13000); check(!duct_available && current_time == 9, "escalated cooldown outlasts the base cooldown");
        advance(2000); check(duct_available, "escalated cooldown completes");
    } else if (test == "switch_tag_share_count") {
        my["cool_time"] = "5"; my["cool_time_add"] = "5";
        ReadyFunc(); ActivateRunOnce();
        DuctTag("G1P1"); check(use_duct_num == 1 && cooltime == 5, "outside tag still counts one use");
        advance(10000); check(duct_available, "tag cooldown completes");
        pressSwitch(); check(use_duct_num == 2, "switch shares the counter with outside tags");
        advance(10000); check(duct_available, "switch cooldown completes");
        DuctTag("G1P2"); check(use_duct_num == 3 && cooltime == 10, "switch use pushes the next tag onto the next step");
        advance(15000); check(duct_available, "escalated cooldown completes");
        MmmmOpen(); advance(4000);
        check(use_duct_num == 3 && duct_available && !cooltime_timer.isEnabled(cooltime_timer_id),
              "admin opening is still not counted and creates no cooldown");
        SettingFunc(); check(use_duct_num == 0 && cooltime == 0, "reset clears the use counter");
    } else if (test == "blocked_open_not_counted") {
        my["cool_time"] = "5"; my["cool_time_add"] = "5";
        ReadyFunc(); ActivateRunOnce();
        EnterTaggerMode(); pressSwitch();
        check(relay == LOW && use_duct_num == 0, "blockaded switch opens nothing and counts nothing");
        ExitTaggerMode(); pressSwitch(); check(use_duct_num == 1, "switch counts again after the blockade");
        advance(4000); pressSwitch();
        check(relay == LOW && use_duct_num == 1, "switch during cooldown does not count a use");
        advance(6000); check(duct_available && use_duct_num == 1, "cooldown completes without extra counting");
    } else if (test == "server_cooltime_fallback") {
        my["cool_time"] = "0"; my["cool_time_add"] = "0";
        cooltime_set = 30; cooltime_add = 30;
        ReadyFunc(); ActivateRunOnce();
        check(cooltime_set == 30 && cooltime == 30, "missing or zero cool_time keeps the firmware default");
        check(cooltime_add == 0, "cool_time_add of zero is honoured as no escalation");
        my["cool_time"] = "7"; ReadyFunc(); ActivateRunOnce();
        check(cooltime_set == 7 && cooltime == 7, "server cool_time is adopted when set");
    } else if (test.rfind("audio_", 0) == 0) {
        const int seconds = std::stoi(test.substr(6));
        const bool minutes = seconds >= 60;
        std::vector<String> expected = {
            "play:1:3",
            "play:" + String(minutes ? "2:" : "3:") + std::to_string(minutes ? seconds / 60 : seconds),
            minutes ? "play:1:4" : "play:1:5"
        };
        cooltime = seconds + 7; current_time = 7;
        CooltimeMp3();
        check(audioEvents == expected, "normal cooldown queues original intro, remaining amount and units");
        check(cooltime == seconds + 7 && current_time == 7, "announcement preserves countdown values");
        audioEvents.clear(); expected[0] = "play:4:2";
        RemainingTimeMp3(4, 2, seconds);
        check(audioEvents == expected, "shared helper queues supplied blockade intro with original amount and units");
    } else if (test == "blockade_remaining_audio") {
        EnterTaggerMode(); expectBlockadeAudio(30);
        advance(10000); expectBlockadeAudio(20);
        advance(20000); expectBlockadeAudio(0);
        advance(5000); expectBlockadeAudio(0);
        check(tagger_mode, "temporary announcement duration does not auto-release server blockade");
        check(has2wifi.states.empty(), "countdown expiration does not send release to server");
    } else if (test == "blockade_reentry_audio") {
        EnterTaggerMode(); advance(10000); EnterTaggerMode(); expectBlockadeAudio(20);
        ExitTaggerMode(); EnterTaggerMode(); expectBlockadeAudio(30);
        advance(999); expectBlockadeAudio(30);
        advance(1); expectBlockadeAudio(29);
    } else if (test == "blockade_button_preserves_close") {
        openNormal(); advance(1000); EnterTaggerMode();
        const int pendingClose = duct_close_timer_id;
        // Defensive direct call while the normal opening is still waiting to close.
        DuctOpen(true);
        check(duct_close_timer_id == pendingClose && duct_close_timer.isEnabled(pendingClose),
              "blocked feedback leaves original close callback intact");
        advance(3000);
        check(relay == LOW && switch_available && tagger_mode,
              "original four second close still fires under blockade");
        check(cooltime_timer.isEnabled(cooltime_timer_id) && current_time == 0,
              "original close still prepares frozen normal cooldown");
    } else if (test == "open_audio_paths") {
        // 펌웨어 하네스 main은 mp3_available을 켜지 않는다. Mp3PlayLargeFolder가 이 플래그로 조기 반환하므로 켜 준다.
        mp3_available = true;
        openNormal();
        check(audioEvents == std::vector<String>{"play:9:712"}, "outside tag plays the outside opening line");
        advance(4000); finished();
        audioEvents.clear(); pressSwitch();
        check(relay == HIGH && audioEvents == std::vector<String>{"play:9:719"}, "inside switch plays the inside opening line");
        advance(4000); finished();
        audioEvents.clear(); DuctOpen();
        check(relay == HIGH && audioEvents == std::vector<String>{"play:9:712"}, "server manage open plays the outside opening line");
        advance(4000); finished();
        audioEvents.clear(); MmmmOpen();
        check(relay == HIGH && audioEvents == std::vector<String>{"play:9:712"}, "admin card plays the outside opening line");
        advance(4000); check(relay == LOW && !mmmm_open, "admin opening closes");
    } else if (test == "server_activate") {
        openNormal(); advance(6000); check(!duct_available && current_time == 2, "cooldown in progress");
        has2wifi.states.clear();
        ServerActivate();
        check(duct_available && current_time == 0 && !cooltime_timer.isEnabled(cooltime_timer_id),
              "server activate ends the cooldown immediately");
        check(use_duct_num == 1, "server activate keeps the use counter");
        check(pixels_line.color == std::array<int, 3>{255, 255, 0}, "server activate paints yellow");
        check(has2wifi.states == std::vector<String>{"activate"}, "server activate reports activate once");
        has2wifi.states.clear(); ServerActivate();
        check(duct_available && has2wifi.states.empty(), "server activate while available changes nothing");
        DuctTag("G1P2"); check(relay == HIGH && use_duct_num == 2, "duct opens again after forced activation");
        advance(4000);
        check(relay == LOW && cooltime_timer.isEnabled(cooltime_timer_id),
              "the timer slot ServerActivate deleted is cleanly reusable for the next cooldown");
        finished();
    } else if (test == "server_activate_door_open") {
        openNormal(); advance(1000); ServerActivate();
        check(!duct_available && relay == HIGH, "server activate is ignored while the door is open");
        advance(3000); check(relay == LOW && cooltime_timer.isEnabled(cooltime_timer_id), "door still closes into a normal cooldown");
        finished();
        openNormal(); advance(6000); MmmmOpen(); advance(500); ServerActivate();
        check(mmmm_open && !duct_available, "server activate is ignored during admin opening");
        advance(3500);
        check(!mmmm_open && !duct_available && cooltime_timer.isEnabled(cooltime_timer_id), "admin close restores the cooldown");
        finished();
    } else if (test == "server_activate_blockade") {
        openNormal(); advance(6000); EnterTaggerMode(); has2wifi.states.clear();
        ServerActivate();
        check(!tagger_mode && duct_available && current_time == 0, "server activate releases blockade and cooldown together");
        check(!has2wifi.states.empty() && has2wifi.states.back() == "activate", "server reports activate after release");
        check(pixels_line.color == std::array<int, 3>{255, 255, 0}, "released duct is yellow");
        EnterTaggerMode(); has2wifi.states.clear(); ServerActivate();
        check(!tagger_mode && duct_available && has2wifi.states == std::vector<String>{"activate"},
              "available blockade release keeps existing exit behaviour");
    } else if (test == "blockade_left_time") {
        my["left_time"] = "3";   // 이전 봉쇄에서 남아 있던 값
        EnterTaggerMode(); TaggerLeftTimeUpdate();
        check(TaggerRemainingSeconds() == 30, "a value the server has not acknowledged yet is ignored");
        tagger_server_confirmed = true; my["left_time"] = ""; TaggerLeftTimeUpdate();
        check(TaggerRemainingSeconds() == 30, "missing left_time keeps the 30 second default");
        my["left_time"] = "25"; TaggerLeftTimeUpdate();
        check(TaggerRemainingSeconds() == 25, "server left_time replaces the default");
        advance(3000); check(TaggerRemainingSeconds() == 22, "remaining time counts down from the last received value");
        TaggerLeftTimeUpdate();   // 같은 값(25) 재수신 - 수신 시각을 다시 찍으면 안 된다
        check(TaggerRemainingSeconds() == 22, "repeated identical left_time does not rewind the countdown");
        expectBlockadeAudio(22);
        my["left_time"] = "10"; TaggerLeftTimeUpdate(); advance(500);
        check(TaggerRemainingSeconds() == 10, "newer left_time wins and partial seconds round up");
        my["left_time"] = "0"; TaggerLeftTimeUpdate();
        check(TaggerRemainingSeconds() == 10, "zero left_time does not overwrite the last value");
        advance(12000);
        check(TaggerRemainingSeconds() == 0 && tagger_mode, "expired server value announces zero but never self-releases");
        my["left_time"] = "90"; TaggerLeftTimeUpdate();
        check(TaggerRemainingSeconds() == 90, "a blockade longer than a minute is accepted");
        audioEvents.clear(); tag["role"] = "player";
        uint8_t minute_card[32] = {'G', '1', 'P', '1'};
        CardChecking(minute_card);
        check(audioEvents == std::vector<String>{"play:4:2", "play:2:1", "play:1:4"},
              "blockade over a minute announces minutes instead of seconds");
        ExitTaggerMode(); my["left_time"] = ""; EnterTaggerMode(); TaggerLeftTimeUpdate();
        check(TaggerRemainingSeconds() == 30, "re-entering the blockade forgets the previous server value");
        ExitTaggerMode(); my["left_time"] = "7"; TaggerLeftTimeUpdate();
        check(!tagger_left_time_valid, "left_time is ignored outside the blockade");
    } else if (test == "blockade_left_time_reset") {
        // 봉쇄 중 게임이 리셋되면 DataChange 는 tagger_mode 만 내리고 서버 레코드(device_state,
        // left_time)는 그대로 둔다. 그 뒤 덕트킬로 시작된 새 봉쇄가 이전 봉쇄의 확인과 값을
        // 물려받으면 안 된다.
        EnterTaggerMode(); tagger_server_confirmed = true;
        my["left_time"] = "40"; TaggerLeftTimeUpdate();
        check(TaggerRemainingSeconds() == 40, "a confirmed blockade uses the server value");
        tagger_mode = false; ActivateRunOnce();   // 봉쇄 중 게임 리셋 (back 없이)
        EnterTaggerMode(); TaggerLeftTimeUpdate(); // 다음 덕트킬
        check(TaggerRemainingSeconds() == 30 && !tagger_left_time_valid,
              "a new blockade does not inherit the previous confirmation");
        tagger_server_confirmed = true; TaggerLeftTimeUpdate();
        check(TaggerRemainingSeconds() == 40, "the new blockade accepts the value once the server confirms it");
    } else return 2;
    std::cout << "PASS " << test << '\n';
}
