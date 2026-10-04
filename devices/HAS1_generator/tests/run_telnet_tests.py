#!/usr/bin/env python3
"""Host regression tests for the real bounded sender and history (no hardware)."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'telnet.ino').read_text()


def function(name):
    start = SOURCE.index(name)
    opening = SOURCE.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[start:end]


PREAMBLE = r'''
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "telnet_log_buffer.h"
#define MSG_DONTWAIT 64
#define portENTER_CRITICAL(x) do { (void)(x); } while(0)
#define portEXIT_CRITICAL(x) do { (void)(x); } while(0)
static int telnetLogMux;
static TelnetLogBuffer<8192> telnetHistory;
static bool telnetSessionActive = true;
static uint64_t telnetPosition = 0, telnetReplayEnd = 0;
static bool telnetReplayEnded = false;
static char telnetControl[1024];
static size_t telnetControlLength = 0, telnetControlSent = 0;
struct Client {
  int descriptor = 7;
  bool stopped = false;
  int fd() { return descriptor; }
  void stop() { stopped = true; descriptor = -1; }
} telnetClient;
static std::string output;
static std::vector<int> sendPlan;
static size_t planPosition = 0, calls = 0;
static int send(int descriptor, const void* buffer, size_t size, int flags) {
  assert(descriptor == 7 && flags == MSG_DONTWAIT && size <= 256);
  ++calls;
  int result = planPosition < sendPlan.size() ? sendPlan[planPosition++] : static_cast<int>(size);
  if(result < 0) { errno = -result; return -1; }
  result = std::min(result, static_cast<int>(size));
  output.append(static_cast<const char*>(buffer), result);
  return result;
}
static void reset() {
  telnetHistory = TelnetLogBuffer<8192>();
  telnetPosition = telnetReplayEnd = 0;
  telnetReplayEnded = false;
  telnetSessionActive = true;
  telnetControlLength = telnetControlSent = 0;
  telnetClient = Client();
  output.clear(); sendPlan.clear(); planPosition = calls = 0;
}
static void append(const std::string& value) {
  telnetHistory.append(reinterpret_cast<const uint8_t*>(value.data()), value.size());
}
static void prefix(const char* value) {
  strcpy(telnetControl, value); telnetControlLength = strlen(value); telnetControlSent = 0;
}
static std::string uart;
static int previousVprintf(const char* fmt, va_list args) {
  char buffer[4096]; int size = vsnprintf(buffer, sizeof(buffer), fmt, args);
  uart.append(buffer); return size;
}
static int (*telnetPreviousVprintf)(const char*, va_list) = previousVprintf;
static char telnetRetainedCrash[200];
static char g_crash_msg[200];
static bool g_crash_telnet_pending;
'''

TESTS = r'''
static int idfPrint(const char* fmt, ...) {
  va_list arguments; va_start(arguments, fmt);
  int result = TelnetVprintf(fmt, arguments); va_end(arguments); return result;
}
static std::string historyText() {
  uint8_t bytes[8192];
  size_t count = telnetHistory.copy(telnetHistory.oldest(), bytes, sizeof(bytes));
  return std::string(reinterpret_cast<char*>(bytes), count);
}
static void drain(unsigned limit = 100) {
  for(unsigned i=0;i<limit;++i) {
    size_t before = calls; TelnetDrain(); assert(calls - before <= 4);
  }
}
int main() {
  const std::string end = "\r\n[Telnet] replay end / live output\r\n";
  reset(); append("boot\n"); telnetReplayEnd = telnetHistory.end(); prefix("SNAPSHOT\n"); append("live\n");
  sendPlan = {2, 1, 1, 1, 2, 1, 1}; drain();
  assert(output == "SNAPSHOT\nboot\n" + end + "live\n");
  assert(telnetPosition == telnetHistory.end());
  puts("PASS partial prefix/data writes, replay boundary and live ordering");

  reset(); append("pending"); telnetReplayEnd = telnetHistory.end();
  sendPlan = {-EAGAIN}; TelnetDrain(); assert(calls == 1 && telnetPosition == 0 && output.empty());
  sendPlan = {0}; planPosition = 0; TelnetDrain(); assert(telnetPosition == 0);
  sendPlan = {-EINTR}; planPosition = 0; TelnetDrain(); assert(!telnetClient.stopped);
  sendPlan.clear(); drain(); assert(output == "pending" + end);
  puts("PASS EAGAIN/EINTR/zero send preserve cursor for a later loop");

  reset(); append(std::string(3000, 'a')); telnetReplayEnd = telnetHistory.end();
  sendPlan = {7, -EAGAIN}; TelnetDrain(); assert(telnetPosition == 7);
  append(std::string(10000, 'b')); drain();
  assert(output.find("[Telnet] log overflow: 4801 bytes skipped") != std::string::npos);
  assert(output.substr(output.size()-8192) == std::string(8192,'b'));
  assert(telnetPosition == telnetHistory.end());
  puts("PASS overwritten pending replay is explicitly marked; latest bytes preserved");

  reset(); append(std::string(8192,'x')); telnetReplayEnd = telnetHistory.end(); TelnetDrain();
  assert(calls == 4 && output.size() == 1024 && telnetPosition == 1024);
  puts("PASS per-loop send budget is four attempts / 1024 bytes");

  reset(); append("failed"); telnetReplayEnd = telnetHistory.end();
  sendPlan = {-ECONNRESET}; TelnetDrain(); assert(telnetClient.stopped && telnetPosition == 0);
  reset(); append("new session"); telnetReplayEnd = telnetHistory.end(); drain();
  assert(output == "new session" + end);
  puts("PASS fatal socket failure stops client; sender can restart with clean session state");

  reset(); uart.clear(); std::string longLine(900,'Q');
  assert(idfPrint("%s",longLine.c_str()) == 900);
  assert(uart == longLine);
  std::string idf = historyText();
  assert(idf.substr(0,511) == longLine.substr(0,511));
  assert(idf.find("ESP-IDF log truncated at 511 bytes") != std::string::npos);
  puts("PASS bounded IDF replay marks truncation while original UART sink gets full message");

  reset(); strcpy(g_crash_msg,"reason=TASK_WDT fn=WirePollMain crash#2"); g_crash_telnet_pending=true;
  TelnetPreserveCrashReport(); assert(!g_crash_telnet_pending);
  strcpy(g_crash_msg,"URL_sanitized"); append(std::string(20000,'z'));
  assert(std::string(telnetRetainedCrash) == "reason=TASK_WDT fn=WirePollMain crash#2");
  puts("PASS reset report remains original after URL mutation and history wrap");
  return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='generator-telnet-test-') as tmp:
    source = Path(tmp) / 'test.cpp'
    source.write_text(PREAMBLE + '\n' + '\n'.join(function(name) for name in (
        'static void TelnetRemember(', 'static int TelnetVprintf(',
        'void TelnetPreserveCrashReport(', 'static void TelnetDrain(')) + TESTS)
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CXX','c++'), '-std=c++11', '-Wall', '-Wextra',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(ROOT), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
