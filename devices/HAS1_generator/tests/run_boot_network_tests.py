#!/usr/bin/env python3
"""Execute production setup/loop and TelnetInit with an uninitialized TCP/IP mock.
Does not simulate FreeRTOS, lwIP internals or physical ESP32 boot timing.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = '\n'.join(line for line in (ROOT / 'HAS1_generator.ino').read_text().splitlines()
                   if not line.startswith('#include'))
telnet = (ROOT / 'telnet.ino').read_text()
start = telnet.index('void TelnetInit() {')
end = telnet.index('\n}', start) + 2
init = telnet[start:end]
prefix = r'''
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
static bool initializeOk = true, tcpReady = false, workerStarted = false;
static unsigned initCalls = 0, socketCalls = 0, workerCalls = 0;
static unsigned networkLoops = 0, telnetLoops = 0, diagnosticLoops = 0;
static unsigned yieldCalls = 0, gameLoops = 0, bleBegins = 0, blePolls = 0;
static unsigned otaSetups = 0, connectCalls = 0, failureLogs = 0;
static unsigned watchdogFeeds = 0;
static std::vector<std::string> events;
struct Net {
 bool begin() {
  assert(!workerStarted && socketCalls == 0);
  ++initCalls; events.emplace_back("tcp-init");
  tcpReady = initializeOk;
  return tcpReady;
 }
} Network;
struct Logger {
 void begin(unsigned) {}
 template<class T> void print(const T&) {}
 template<class T> void println(const T&) {}
 void println(const char* text) {
  if (std::string(text) == "[NET] TCP/IP initialization failed; network startup disabled") {
   ++failureLogs;
  }
 }
} Serial, DebugSerial;
struct Wifi { const char* localIP() { assert(tcpReady); return "0.0.0.0"; } } WiFi;
struct Server {
 void begin() { assert(tcpReady); ++socketCalls; events.emplace_back("socket"); }
 void setNoDelay(bool) { assert(tcpReady); }
} telnetServer;
void DiagnosticHttpHook() {}
struct Has {
 void SetHttpDiagnosticHook(void(*)()) {}
 void EnableBoundedResponseBody() {}
 void SetDebugPrint(Logger*) {}
 void BeginConnection(const char*) { ++connectCalls; events.emplace_back("configure"); }
} has2wifi;
struct JsonValue { const char* operator|(const char* s) { return s; } };
struct Json { JsonValue operator[](const char*) { return {}; } } my;
void GeneratorNetworkSend(const char*,const char*,const char*) { assert(tcpReady); }
struct Ota {
 void setLogStream(Logger&) { ++otaSetups; }
 template<class T> void setOnSuccess(T) {}
 template<class T> void setOnSkip(T) {}
 void setPartitionUpdate(const char*,const char*,const char*,int) {}
} ota;
void DiagnosticInit() {}
void CrashReportInit() {}
void TelnetPreserveCrashReport() {}
void NeopixelInit() {}
void RfidInit() {}
void EncoderInit() {}
void WireInit() {}
void Mp3_Init() {}
void TimerInit() {}
void WaitFunc() {}
void (*ptrCurrentMode)() = nullptr;
void (*ptrRfidMode)() = nullptr;
void GeneratorNetworkBegin() {
 ++workerCalls; workerStarted = true; events.emplace_back("worker");
 // Deliberately do not run the worker: setup must be safe even when core 0
 // has not reached WiFi.mode() before main opens the Telnet socket.
}
void DiagnosticPrintf(const char*) {}
void DiagnosticLoop() { ++diagnosticLoops; }
void GeneratorNetworkLoop() { assert(tcpReady && workerStarted); ++networkLoops; }
void TelnetRun() { assert(tcpReady && socketCalls); ++telnetLoops; }
void WireServiceLoop() { ++gameLoops; }
void EncoderLoop() {}
void ContribLoop() {}
void TimerRun() {}
void delay(unsigned ms) { assert(ms == 1); ++yieldCalls; }
void esp_task_wdt_reset() { ++watchdogFeeds; }
namespace Has1BleBeacon {
 void begin() { ++bleBegins; }
 void poll() { ++blePolls; }
}
void TelnetInit();
'''
tests = r'''
int main(int argc, char**) {
 initializeOk = argc == 1;
 setup();
 assert(initCalls == 1);
 assert(ptrCurrentMode == WaitFunc && ptrRfidMode == WaitFunc);
 for (unsigned i = 0; i < 1000; ++i) loop();
 assert(diagnosticLoops == 1000 && initCalls == 1);
 if (initializeOk) {
  const std::vector<std::string> expected{"tcp-init", "configure", "worker", "socket"};
  assert(events == expected);
  assert(connectCalls == 1 && workerCalls == 1 && socketCalls == 1);
  assert(networkLoops == 1000 && telnetLoops == 1000 && gameLoops == 1000);
  assert(bleBegins == 1 && blePolls == 1000 && otaSetups == 1);
  assert(yieldCalls == 0 && failureLogs == 0 && watchdogFeeds == 0);
  puts("PASS: setup initializes TCP/IP before deferred worker and actual Telnet socket; loop remains active");
 } else {
  assert(events == std::vector<std::string>{"tcp-init"});
  assert(connectCalls == 0 && workerCalls == 0 && socketCalls == 0);
  assert(networkLoops == 0 && telnetLoops == 0 && gameLoops == 0);
  assert(bleBegins == 0 && blePolls == 0 && otaSetups == 0);
  assert(yieldCalls == 1000 && failureLogs == 1 && watchdogFeeds == 1000);
  puts("PASS: TCP/IP init failure never starts worker/sockets/gameplay; diagnostic loop yields without retries or reboots");
 }
}
'''
with tempfile.TemporaryDirectory(prefix='generator-boot-network-') as directory:
    cpp = Path(directory) / 'boot.cpp'
    binary = Path(directory) / 'boot'
    cpp.write_text(prefix + source + init + tests)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    subprocess.run([str(binary), 'init-failure'], check=True)
