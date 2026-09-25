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
#define REVIVAL_RFID_RUNTIME_TRACE 0
static uint64_t fakeTimeUs = 1000000;
uint32_t micros() { return static_cast<uint32_t>(fakeTimeUs); }
uint32_t millis() { return static_cast<uint32_t>(fakeTimeUs / 1000); }
void delay(unsigned long ms) { fakeTimeUs += uint64_t(ms) * 1000; }
void delayMicroseconds(unsigned int us) { fakeTimeUs += us; }
struct FakeSerial {
  std::vector<std::string> lines;
  size_t write(const uint8_t* data, size_t size) { lines.emplace_back(reinterpret_cast<const char*>(data), size); return size; }
  void println(const char* text) { lines.emplace_back(text); }
  void printf(const char* format, ...) {
    char out[1024]; va_list args; va_start(args, format);
    int count = vsnprintf(out, sizeof(out), format, args); va_end(args);
    assert(count >= 0 && size_t(count) < sizeof(out)); lines.push_back(out);
  }
} Serial, HardwareDebugSerial;
#include "fake_pn532_reader.h"
#include "sensor_test_state.h"
FakePn532Reader pn532;
enum GainMode { GAIN_CONTACT, GAIN_NEAR, GAIN_FAR };
bool gameplay_tag_latched = true;
std::string gameplay_tag_user = "G9P2";
using String = std::string;
struct DeviceState { const char* operator[](const char* key) { assert(std::string(key) == "device_name"); return "AR"; } } my;
struct FakeWifi {
  unsigned sends = 0;
  void Send(const String& device, const String& field, const String& value) {
    assert(device == "AR" && field == "device_state" && value == "PN532"); ++sends;
  }
} has2wifi;
#include "low_level.inc"
#include "rfid_init.inc"
#include "observe.inc"
static bool scan(uint8_t* data) {
  bool ok = DetectWithGainSwitch(data);
  ObserveGameplayTagOutcome(rfid_last_outcome);
  return ok;
}
static unsigned configCount(uint8_t item, int value = -1) {
  unsigned result = 0;
  for (const auto& command : pn532.commands)
    if (command[1] == item && (value < 0 || command[2] == value)) ++result;
  return result;
}
static void assertHeld() { assert(gameplay_tag_latched && gameplay_tag_user == "G9P2"); }
int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string scenario = argv[1];
  uint8_t data[32] = {};
  if (scenario == "startup_success") {
    RfidInit();
    assert(!rfid_recovery_required && rfid_gain_known && pn532.abortCalls == 0);
    assert(has2wifi.sends == 0 && pn532.versionCalls == 1);
  } else if (scenario == "startup_report_once") {
    pn532.versionResults = {Pn532Result::TransportFault, Pn532Result::TransportFault,
                           Pn532Result::TransportFault, Pn532Result::TransportFault};
    RfidInit();
    assert(has2wifi.sends == 1 && rfid_recovery_required && !rfid_gain_known);
    for (unsigned attempt = 0; attempt < RFID_RECOVERY_MAX_ATTEMPTS; ++attempt) {
      delay(uint32_t(rfid_next_recovery_ms - millis()));
      assert(!RfidEnsureReady(true));
      assert(has2wifi.sends == 1);
    }
    assert(rfid_recovery_locked && pn532.versionCalls == 4);
  } else if (scenario == "unique_near" || scenario == "unique_contact" || scenario == "unique_far") {
    currentGain = scenario == "unique_near" ? GAIN_NEAR : scenario == "unique_contact" ? GAIN_CONTACT : GAIN_FAR;
    assert(!scan(data) && rfid_last_outcome == RfidReadOutcome::NoTarget);
    assert(pn532.uidCalls == 3 && configCount(0x0A) == 2);
    for (int cfg : {0x09, 0x19, 0x49}) assert(configCount(0x0A, cfg) <= 1);
    assert(configCount(1, 0) == 1 && configCount(1, 1) == 1);
  } else if (scenario == "normal_timed_absence") {
    for (int i = 0; i < 6; ++i) pn532.targetDurationsUs.push_back(80000);
    for (int i = 0; i < 8; ++i) pn532.configDurationsUs.push_back(3000);
    const auto started = fakeTimeUs;
    assert(!scan(data) && rfid_last_outcome == RfidReadOutcome::NoTarget);
    assert(fakeTimeUs - started == 264000); // Three UID attempts, two gains, OFF/ON and two 6ms settling waits.
    assertHeld(); assert(gameplay_tag_missing && gameplay_tag_miss_count == 1);
    assert(configCount(1, 0) == 1 && configCount(1, 1) == 1);
    delay(RFID_REARM_ABSENT_MS);
    assert(!scan(data) && rfid_last_outcome == RfidReadOutcome::NoTarget);
    assert(!gameplay_tag_latched && gameplay_tag_user.empty());
    assert(!rfid_recovery_required && rfid_gain_known);
  } else if (scenario == "tight_clean_absence") {
    pn532.targetDurationsUs = {145000, 145000, 145000};
    pn532.configDurationsUs = {3000, 3000};
    const auto started = fakeTimeUs;
    assert(!scan(data) && rfid_last_outcome == RfidReadOutcome::NoTarget);
    assert(fakeTimeUs - started == 441000 && configCount(1) == 0);
    assert(!rfid_recovery_required && rfid_gain_known && gameplay_tag_missing);
  } else if (scenario == "rf_off_failure" || scenario == "rf_on_failure") {
    pn532.ackResults = scenario == "rf_off_failure" ? std::deque<bool>{true, true, false} :
                                                                  std::deque<bool>{true, true, true, false};
    assert(!scan(data) && rfid_last_outcome == RfidReadOutcome::TransportFault);
    assert(pn532.uidCalls == 3 && configCount(1, 0) == 1);
    assert(configCount(1, 1) == (scenario == "rf_off_failure" ? 0 : 1));
    assert(rfid_recovery_required && !rfid_gain_known && !gameplay_tag_missing); assertHeld();
  } else if (scenario == "gain_failure") {
    pn532.ackResults = {false};
    assert(!scan(data));
    assert(pn532.uidCalls == 1 && pn532.operations.size() == 2 && configCount(1) == 0);
    assert(currentGain == GAIN_NEAR && !rfid_gain_known && rfid_recovery_required);
    assert(rfid_last_outcome == RfidReadOutcome::TransportFault); assertHeld();
  } else if (scenario == "payload_error") {
    gameplay_tag_missing = true; gameplay_tag_miss_count = 1; gameplay_tag_missing_since_ms = 1;
    pn532.uidResults = {true}; pn532.pageResults = {Pn532Result::TagError};
    assert(!scan(data) && rfid_last_outcome == RfidReadOutcome::TagReadFailed);
    assert(pn532.uidCalls == 1 && pn532.readCalls == 1 && pn532.commands.empty());
    assert(!rfid_recovery_required && !gameplay_tag_missing); assertHeld();
  } else if (scenario == "transport_failure") {
    pn532.targetResults = {Pn532Result::TransportFault};
    assert(!scan(data) && rfid_last_outcome == RfidReadOutcome::TransportFault);
    assert(pn532.operations.size() == 1 && rfid_recovery_required && !rfid_gain_known);
    for (int i = 0; i < 100; ++i) assert(!RfidEnsureReady(true));
    assert(pn532.operations.size() == 1); assertHeld();
  } else if (scenario == "scan_budget" || scenario == "scan_budget_wrap") {
    if (scenario == "scan_budget_wrap") fakeTimeUs = (uint64_t(UINT32_MAX) - 100) * 1000;
    pn532.targetDurationsUs = {200000, 200000, 200000};
    const auto started = fakeTimeUs;
    assert(!scan(data));
    assert(rfid_last_outcome == RfidReadOutcome::BudgetExceeded);
    assert(fakeTimeUs - started <= uint64_t(RFID_SCAN_BUDGET_MS + 1) * 1000);
    assert(!gameplay_tag_missing); assertHeld();
  } else if (scenario == "recovery_bounded" || scenario == "recovery_wrap") {
    if (scenario == "recovery_wrap") fakeTimeUs = (uint64_t(UINT32_MAX) - 500) * 1000;
    pn532.targetResults = {Pn532Result::TransportFault}; scan(data);
    pn532.versionResults = {Pn532Result::TransportFault, Pn532Result::TransportFault, Pn532Result::TransportFault};
    for (unsigned attempt = 1; attempt <= RFID_RECOVERY_MAX_ATTEMPTS; ++attempt) {
      unsigned before = pn532.versionCalls;
      uint32_t remaining = uint32_t(rfid_next_recovery_ms - millis());
      const uint32_t expectedBackoff[] = {1000, 5000, 30000};
      assert(remaining == expectedBackoff[attempt - 1]);
      delay(remaining - 1); assert(!RfidEnsureReady(true)); assert(pn532.versionCalls == before);
      delay(1); const auto started = fakeTimeUs;
      assert(!RfidEnsureReady(true));
      assert(pn532.versionCalls == before + 1 && rfid_recovery_attempts == attempt);
      assert(fakeTimeUs - started <= uint64_t(RFID_RECOVERY_BUDGET_MS) * 1000);
      assertHeld();
    }
    assert(rfid_recovery_locked && pn532.abortCalls == RFID_RECOVERY_MAX_ATTEMPTS);
    const auto operations = pn532.operations.size();
    for (int i = 0; i < 100; ++i) { delay(60000); assert(!RfidEnsureReady(true)); }
    assert(pn532.operations.size() == operations);
  } else if (scenario == "recovery_budget") {
    rfid_recovery_required = true; rfid_gain_known = false;
    pn532.samDurationsUs = {90000}; pn532.versionDurationsUs = {90000};
    pn532.configDurationsUs = {90000, 90000, 90000};
    const auto started = fakeTimeUs;
    assert(!RfidInitializeHardware(true));
    assert(rfid_recovery_required && !rfid_gain_known);
    assert(fakeTimeUs - started <= uint64_t(RFID_RECOVERY_BUDGET_MS) * 1000);
    assertHeld();
  } else if (scenario == "recovery_success") {
    gameplay_tag_missing = true; gameplay_tag_miss_count = 1; gameplay_tag_missing_since_ms = 1;
    rfid_recovery_required = true; rfid_gain_known = false;
    assert(!RfidEnsureReady(true)); // Recovery consumes this iteration; scan is deferred.
    assert(RfidEnsureReady(true));
    assert(!rfid_recovery_required && rfid_gain_known && currentGain == GAIN_NEAR);
    assert(!gameplay_tag_missing && gameplay_tag_miss_count == 0); assertHeld();
    assert(pn532.retryCalls == 1 && pn532.lastRetries == RFID_ACTIVATION_RETRIES && pn532.abortCalls == 1);
    assert(configCount(0x0A, 0x19) == 1 && configCount(1, 1) == 1);
    ObserveGameplayTagOutcome(RfidReadOutcome::NoTarget); assertHeld();
    delay(RFID_REARM_ABSENT_MS); ObserveGameplayTagOutcome(RfidReadOutcome::NoTarget);
    assert(!gameplay_tag_latched);
  } else if (scenario == "approval_blocks_recovery") {
    revival_approval_pending = true; rfid_recovery_required = true; rfid_gain_known = false;
    for (int i = 0; i < 20; ++i) {
      delay(1000); assert(!RfidEnsureReady(true)); assert(!RfidEnsureReady(false));
    }
    assert(pn532.operations.empty() && rfid_recovery_attempts == 0); assertHeld();
    revival_approval_pending = false;
    assert(!RfidEnsureReady(true)); // Recovery only, no second scan in this iteration.
    assert(RfidEnsureReady(true));
    assert(pn532.versionCalls == 1 && !rfid_recovery_required);
  } else if (scenario == "health_deferred_usb") {
    pn532.targetResults = {Pn532Result::TransportFault};
    assert(!scan(data));
    assert(Serial.lines.empty() && HardwareDebugSerial.lines.empty());
    RfidFlushHealthLog();
    assert(Serial.lines.empty() && HardwareDebugSerial.lines.size() == 1);
    const auto& line = HardwareDebugSerial.lines.front();
    assert(line.find("[RFID_HEALTH] state=fault") == 0);
    assert(line.find("status=0x05") != std::string::npos && line.find("G9P2") == std::string::npos);
    for (int i = 0; i < 10; ++i) { assert(!RfidEnsureReady(true)); RfidFlushHealthLog(); }
    assert(HardwareDebugSerial.lines.size() == 1);
  } else if (scenario == "unknown_breaks_absence") {
    for (auto outcome : {RfidReadOutcome::TagReadFailed, RfidReadOutcome::TransportFault,
                        RfidReadOutcome::BudgetExceeded, RfidReadOutcome::Unavailable}) {
      gameplay_tag_latched = true; gameplay_tag_user = "G9P2";
      gameplay_tag_missing = false; gameplay_tag_miss_count = 0;
      ObserveGameplayTagOutcome(RfidReadOutcome::NoTarget);
      delay(RFID_REARM_ABSENT_MS + 1000);
      ObserveGameplayTagOutcome(outcome);
      assert(!gameplay_tag_missing && gameplay_tag_miss_count == 0); assertHeld();
      ObserveGameplayTagOutcome(RfidReadOutcome::NoTarget); assertHeld();
      delay(RFID_REARM_ABSENT_MS - 1); ObserveGameplayTagOutcome(RfidReadOutcome::NoTarget); assertHeld();
      delay(1); ObserveGameplayTagOutcome(RfidReadOutcome::NoTarget); assert(!gameplay_tag_latched);
    }
  } else if (scenario == "upload_blank_select") {
    uint8_t uid[10], length = 0;
    pn532.uidResults = {false, true}; // Select through gain fallback, without requiring card content.
    pn532.pageResults = {Pn532Result::TagError};
    assert(RfidUploadSelect(uid, length) == Pn532Result::Ok && length == 7);
    assert(pn532.uidCalls == 2 && pn532.readCalls == 0 && !rfidUploadUidOnly);
    pn532.uidResults = {true};
    assert(!scan(data) && pn532.readCalls == 1); // Normal gameplay still reads page 7.
  } else if (scenario == "upload_absence") {
    uint8_t uid[10], length = 10;
    assert(RfidUploadSelect(uid, length) == Pn532Result::NoTarget && length == 0);
    assert(pn532.uidCalls == 3 && pn532.readCalls == 0 && !rfidUploadUidOnly);
  } else if (scenario == "upload_recovery") {
    uint8_t uid[10], length = 10;
    rfid_recovery_required = true; rfid_gain_known = false;
    assert(RfidUploadSelect(uid, length) == Pn532Result::Deadline && length == 0);
    assert(pn532.uidCalls == 0 && pn532.versionCalls == 1); // Recovery has its own loop budget.
  } else if (scenario == "upload_fault") {
    RfidUploadObserve(Pn532Result::TagError);
    assert(!rfid_recovery_required);
    RfidUploadObserve(Pn532Result::Deadline);
    assert(rfid_recovery_required && !rfid_gain_known);
    assertHeld();
  } else assert(false && "Unknown recovery scenario");
  assert(has2wifi.sends == (scenario == "startup_report_once" ? 1 : 0));
  std::cout << "PASS " << scenario << '\n';
}
