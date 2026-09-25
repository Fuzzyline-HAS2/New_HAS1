#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <vector>

#define REVIVAL_RFID_DIAGNOSTICS 0
#define FIRMWARE_VER 67
#define RFID_DETECT_TIMEOUT_MS 250
#define RFID_ACTIVATION_RETRIES 10
#define RFID_DEBOUNCE_MS 300
#define REVIVAL_ADMIN_POLL_MS 1000
#define PN532_MIFARE_ISO14443A 0
#include "rfid_runtime_trace.h"
#include "card_upload.h"

static uint64_t fakeTimeUs = 1000;
uint32_t micros() { return static_cast<uint32_t>(fakeTimeUs); }
uint32_t millis() { return static_cast<uint32_t>(fakeTimeUs / 1000); }
void delay(unsigned long ms) { fakeTimeUs += uint64_t(ms) * 1000; }
void delayMicroseconds(unsigned int us) { fakeTimeUs += us; }

struct FakeSerial {
  std::string output;
  uint32_t writeDelayUs = 0;
  size_t write(const uint8_t *data, size_t length) {
    output.append(reinterpret_cast<const char *>(data), length);
    fakeTimeUs += writeDelayUs;
    return length;
  }
  void write(char value) { output += value; }
  void print(char value) { output += value; }
  void print(const char *value) { output += value; }
  void println(char value) { output += value; output += '\n'; }
  void println(const char *value) { output += value; output += '\n'; }
  void printf(const char *format, ...) {
    char buffer[8192];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    assert(length >= 0 && static_cast<size_t>(length) < sizeof(buffer));
    output += buffer;
  }
} Serial;
FakeSerial HardwareDebugSerial;

// Capture every PN532 operation, including arguments and command bytes. The
// runner compares this transcript between separately compiled trace-off/on
// programs so instrumentation cannot quietly alter reader commands or retries.
#include "fake_pn532_reader.h"
#include "sensor_test_state.h"
FakePn532Reader pn532;
enum GainMode { GAIN_CONTACT, GAIN_NEAR, GAIN_FAR };

#include "low_level.inc"
#include "runtime_trace.inc"

// Minimal collaborators for the actual production scan callers and loop().
using String = std::string;

static bool revival_approval_polled_this_loop = false;
static uint32_t revival_approval_last_admin_poll_ms = 0;
static bool rfid_tag = false;
static int rfid_timer_id = 0;
static bool activate_bool = true;
static unsigned int observedCalls = 0;
static unsigned int cardCalls = 0;
static uint32_t cardDelayUs = 0;
static uint32_t telnetDelayUs = 0;
static uint32_t timerDelayUs = 0;
static uint32_t neoDelayUs = 0;
struct FakeTimer {
  unsigned int calls = 0;
  int setTimeout(unsigned long milliseconds, void (*callback)()) {
    assert(milliseconds == 300 && callback);
    return ++calls;
  }
} rfid_timer;
struct FakeState {
  const char *operator[](const char *) { return "activate"; }
} my;
void RfidTagTimerFunc() { rfid_tag = false; }
void ObserveGameplayTagOutcome(RfidReadOutcome outcome) { assert(outcome == RfidReadOutcome::Read); ++observedCalls; }
void CardChecking(uint8_t *) { ++cardCalls; fakeTimeUs += cardDelayUs; }
void AdminCardPollPending();
void AdminCardPollReady();
void RfidLoop();
void TelnetRun() { fakeTimeUs += telnetDelayUs; }
void TimerRun() { fakeTimeUs += timerDelayUs; }
void NeoFunc() { fakeTimeUs += neoDelayUs; }
void ActivateFunc() { RfidLoop(); }
// This suite checks trace parity in ordinary gameplay. Maintenance/exit-gate
// safety is covered by the production session and control-integration suites.
void CardUploadLoop() {}
bool CardUploadBlocksGameplay() { return false; }
void SolenoidOff() {}
#include "scan_callers.inc"
#include "main_loop.inc"

static void printResult(const char *scenario, const std::vector<bool> &results,
                        const uint8_t data[32]) {
  assert(Serial.output.empty() || Serial.output.find("[RFID_HEALTH] ") == 0); // Trace itself remains USB-only.
  std::cout << "[TEST_RESULT] {\"scenario\":\"" << scenario << "\",\"ok\":[";
  for (size_t i = 0; i < results.size(); ++i) {
    if (i) std::cout << ',';
    std::cout << (results[i] ? "true" : "false");
  }
  std::cout << "],\"payload\":[";
  for (int i = 0; i < 4; ++i) {
    if (i) std::cout << ',';
    std::cout << static_cast<unsigned int>(data[i]);
  }
  std::cout << "],\"operations\":[";
  for (size_t i = 0; i < pn532.operations.size(); ++i) {
    if (i) std::cout << ',';
    std::cout << '[';
    for (size_t j = 0; j < pn532.operations[i].size(); ++j) {
      if (j) std::cout << ',';
      std::cout << pn532.operations[i][j];
    }
    std::cout << ']';
  }
  std::cout << "],\"card_calls\":" << cardCalls << ",\"observed_calls\":" << observedCalls
            << ",\"timer_calls\":" << rfid_timer.calls << "}" << std::endl;
  std::cout << HardwareDebugSerial.output;
}

static bool scan(uint8_t data[32], bool autoGain = true, const char *context = "gameplay") {
#if REVIVAL_RFID_RUNTIME_TRACE
  RfidTraceScanBegin(context);
#else
  (void)context;
#endif
  bool ok = autoGain ? DetectWithGainSwitch(data) : DetectAndRead(data);
#if REVIVAL_RFID_RUNTIME_TRACE
  RfidTraceScanEnd(ok, data);
#endif
  return ok;
}

static void beginLoop() {
#if REVIVAL_RFID_RUNTIME_TRACE
  RfidTraceLoopBegin();
#endif
}

static void endLoop() {
#if REVIVAL_RFID_RUNTIME_TRACE
  RfidTraceLoopEnd("activate", "activate");
#endif
}

int main(int argc, char **argv) {
  assert(argc == 2);
  const std::string scenario = argv[1];
  uint8_t data[32] = {};
  std::vector<bool> results;
#if REVIVAL_RFID_RUNTIME_TRACE
  RfidTraceStartup(FIRMWARE_VER, "activate", "activate");
#endif
  assert(pn532.operations.empty());
  if (scenario == "startup") {
    printResult(argv[1], results, data);
    return 0;
  }
  HardwareDebugSerial.output.clear();
  if (scenario.rfind("actual_", 0) == 0) {
    telnetDelayUs = 10000;
    timerDelayUs = 20000;
    neoDelayUs = 30000;
    pn532.uidResults = {true};
    if (scenario == "actual_ready" || scenario == "actual_pending") memcpy(pn532.payload, "MMMM", 4);
    if (scenario == "actual_ready" || scenario == "actual_ready_nonadmin") activate_bool = false;
    if (scenario == "actual_gameplay") cardDelayUs = 2300000;
    if (scenario == "actual_debounce") rfid_tag = true;
    if (scenario == "actual_pending_fault") { rfid_recovery_required = true; rfid_gain_known = false; }
    if (scenario == "actual_pending" || scenario == "actual_pending_skip" || scenario == "actual_pending_recent" || scenario == "actual_pending_fault") {
      revival_approval_pending = true;
      fakeTimeUs = 1000000;
      revival_approval_polled_this_loop = scenario == "actual_pending_skip";
      if (scenario == "actual_pending_recent") revival_approval_last_admin_poll_ms = millis();
    }
    loop();
    const bool skipped = scenario == "actual_debounce" || scenario == "actual_pending_skip" ||
                         scenario == "actual_pending_recent" || scenario == "actual_pending_fault";
    assert(pn532.operations.size() == (skipped ? 0 : 2));
    assert(cardCalls == (scenario == "actual_gameplay" || scenario == "actual_ready" || scenario == "actual_pending" ? 1 : 0));
    assert(observedCalls == (scenario == "actual_gameplay" ? 1 : 0));
    assert(rfid_timer.calls == (scenario == "actual_gameplay" || scenario == "actual_ready" || scenario == "actual_ready_nonadmin" ? 1 : 0));
    printResult(argv[1], results, data);
    return 0;
  }
  if (scenario == "clock_wrap") fakeTimeUs = UINT32_MAX - 50ULL;
  beginLoop();

  if (scenario == "success" || scenario == "clock_wrap" || scenario == "gap" ||
      scenario == "two_scans" || scenario == "admin_payload" || scenario == "emit_delay" ||
      scenario == "post_read") {
    pn532.uidResults = {true, true};
    if (scenario == "admin_payload") memcpy(pn532.payload, "MMMM", 4);
    results.push_back(scan(data));
    assert(results.back());
    assert(pn532.operations.size() == 2);
    assert(data[0] == (scenario == "admin_payload" ? 'M' : 'G'));
    if (scenario == "two_scans") {
      results.push_back(scan(data, false, "admin_pending"));
      assert(results.back() && pn532.operations.size() == 4);
    }
    if (scenario == "post_read") fakeTimeUs += 2300000;
  } else if (scenario == "uid_failure") {
    results.push_back(scan(data));
    assert(!results.back() && pn532.operations.size() == 7);
  } else if (scenario == "page_failure") {
    pn532.uidResults = {true, true, true, true};
    pn532.readResults = {false, false, false, false};
    results.push_back(scan(data));
    assert(!results.back() && pn532.operations.size() == 2);
  } else if (scenario == "gain_switch" || scenario == "gain_ack_failure") {
    pn532.uidResults = {false, true};
    if (scenario == "gain_ack_failure") pn532.ackResults = {false};
    results.push_back(scan(data));
    // Failed configuration stops the sweep and never commits the requested gain.
    assert(results.back() == (scenario == "gain_switch"));
    assert(pn532.operations.size() == (scenario == "gain_switch" ? 4 : 2));
    if (scenario == "gain_ack_failure") assert(currentGain == GAIN_NEAR && !rfid_gain_known);
  } else if (scenario == "fixed_uid_failure") {
    results.push_back(scan(data, false, "admin_pending"));
    assert(!results.back() && pn532.operations.size() == 1);
  } else if (scenario == "stage_overflow") {
#if REVIVAL_RFID_RUNTIME_TRACE
    RfidTraceScanBegin("gameplay");
    for (int i = 0; i < 18; ++i) {
      uint32_t started = micros();
      fakeTimeUs += 1;
      RfidTraceRecord(RFID_TRACE_UID, 0x19, started, false, nullptr);
    }
    RfidTraceScanEnd(false, nullptr);
#endif
  } else if (scenario == "fast_empty" || scenario == "slow_empty") {
#if REVIVAL_RFID_RUNTIME_TRACE
    uint32_t started = micros();
#endif
    fakeTimeUs += scenario == "fast_empty" ? 39999 : 40000;
#if REVIVAL_RFID_RUNTIME_TRACE
    RfidTraceLoopStage(RFID_TRACE_TIMER, started);
#endif
  } else {
    assert(false && "Unknown scenario");
  }

  // Low-level operations only capture into bounded memory; serial output is
  // deferred until the loop has completed all normal production work.
  assert(HardwareDebugSerial.output.empty());
  if (scenario == "emit_delay") HardwareDebugSerial.writeDelayUs = 7000;
  endLoop();
  if (scenario == "gap") {
    // Simulate a busy production loop (network + LEDs) between scans. The
    // reader's own two operations still take only 150us.
    fakeTimeUs += 1500000;
    beginLoop();
#if REVIVAL_RFID_RUNTIME_TRACE
    uint32_t started = micros();
#endif
    fakeTimeUs += 10000;
#if REVIVAL_RFID_RUNTIME_TRACE
    RfidTraceLoopStage(RFID_TRACE_TELNET, started);
    started = micros();
#endif
    fakeTimeUs += 20000;
#if REVIVAL_RFID_RUNTIME_TRACE
    RfidTraceLoopStage(RFID_TRACE_TIMER, started);
    started = micros();
#endif
    fakeTimeUs += 30000;
#if REVIVAL_RFID_RUNTIME_TRACE
    RfidTraceLoopStage(RFID_TRACE_NEO, started);
    started = micros();
#endif
    results.push_back(scan(data));
#if REVIVAL_RFID_RUNTIME_TRACE
    RfidTraceLoopStage(RFID_TRACE_GAME, started);
#endif
    endLoop();
    assert(results.back());
  } else if (scenario == "emit_delay") {
    beginLoop();
    results.push_back(scan(data));
    endLoop();
    assert(results.back());
  }
  printResult(argv[1], results, data);
}
