#!/usr/bin/env python3
"""Actual TimerRun/WifiIntervalFunc before the asynchronous device snapshot arrives."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ['ARDUINOJSON_INCLUDE'])
timer = (ROOT / 'timer.ino').read_text().replace('#include <esp_random.h>', '')
source = r'''
#include <ArduinoJson.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdint>
JsonDocument my;
static uint32_t now = 0;
uint32_t millis() { return now; }
uint32_t esp_random() { return 0; }
struct esp_task_wdt_config_t { unsigned timeout_ms, idle_core_mask; bool trigger_panic; };
void esp_task_wdt_reconfigure(const esp_task_wdt_config_t*) {}
void esp_task_wdt_add(void*) {}
static unsigned feeds = 0, pollCalls = 0, crashCalls = 0, flushes = 0;
void esp_task_wdt_reset() { ++feeds; }
static std::string breadcrumb, crashDevice;
void remember(const char* text) { assert(text); breadcrumb = text; }
#define BREADCRUMB(text) remember(text)
// Desktop printf may tolerate nullptr as "(null)". Reject it explicitly to
// model ESP32 newlib's required non-null %s argument instead of hiding the bug.
int strictStateFormat(char* out, size_t size, const char* format, const char* text) {
 assert(text != nullptr);
 return std::snprintf(out, size, format, text);
}
#define snprintf strictStateFormat
bool GeneratorNetworkRequestPoll() { ++pollCalls; return false; }
bool GeneratorNetworkTakePollResult(bool&) { return false; }
bool GeneratorNetworkPollBusy() { return false; }
void CrashReportSend(const char* device) { assert(device); ++crashCalls; crashDevice = device; }
void CrashNvsFlush() { ++flushes; }
struct Timer {
 unsigned runs = 0;
 int setInterval(int, void(*)()) { return 1; }
 void deleteTimer(int) {}
 void run() { ++runs; }
} GameTimer, BlinkTimer;
int gameTimerId, blinkTimerId, gameTime = 400, blinkTime = 1800;
int gameTimerCnt, encoderValue, starterDecreaseAmount = 1125;
int blinkNeo, blinkColor, color[2] = {}, BLACK = 0;
bool blinkOn;
unsigned g_loop_count;
void NeoLightColor(int, int) {}
void GameTimerFunc();
void BlinkTimerFunc();
'''
source += timer
source += r'''
void tick(const char* expectedState, const char* expectedDevice) {
 now += 3000;
 const unsigned before = pollCalls;
 TimerRun();
 assert(pollCalls == before + 1);
 assert(breadcrumb == expectedState && crashDevice == expectedDevice);
 assert(crashCalls == pollCalls && flushes == pollCalls && feeds == pollCalls);
 assert(GameTimer.runs == pollCalls && BlinkTimer.runs == pollCalls);
}
int main() {
 TimerInit();
 // Server/AP may be unavailable indefinitely: no first snapshot is guaranteed.
 for (unsigned i = 0; i < 1000; ++i) tick("Wifi:ds=uninitialized", "");
 puts("PASS actual TimerRun before first snapshot: 1000 polls, valid breadcrumbs, watchdog feeds, no null formatting");
 my["device_state"] = nullptr;
 my["device_name"] = nullptr;
 tick("Wifi:ds=uninitialized", "");
 my["device_state"] = 123;
 my["device_name"] = 456;
 tick("Wifi:ds=uninitialized", "");
 my["device_state"].to<JsonObject>();
 my["device_name"].to<JsonArray>();
 tick("Wifi:ds=uninitialized", "");
 puts("PASS absent/null/non-string device fields use safe string defaults");
 my["device_state"] = "activate";
 my["device_name"] = "BG";
 tick("Wifi:ds=activate", "BG");
 my["device_state"] = "";
 tick("Wifi:ds=", "BG");
 my["device_state"] = std::string(200, 'x');
 tick("Wifi:ds=xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "BG");
 puts("PASS later valid server snapshot and bounded long/empty state preserve reporting");
}
'''
with tempfile.TemporaryDirectory(prefix='generator-timer-boot-') as directory:
    cpp = Path(directory) / 'timer.cpp'
    binary = Path(directory) / 'timer'
    cpp.write_text(source)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(ROOT), '-I', str(JSON), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
