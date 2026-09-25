#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <vector>

#define REVIVAL_RFID_DIAGNOSTICS 1
#define FIRMWARE_VER 67
#define RFID_DETECT_TIMEOUT_MS 250
#define RFID_ACTIVATION_RETRIES 10
#define RFID_DEBOUNCE_MS 300
#define PN532_MIFARE_ISO14443A 0
#include "rfid_diagnostics.h"

static uint64_t fakeTimeUs = 1000;
uint32_t micros() { return static_cast<uint32_t>(fakeTimeUs); }
uint32_t millis() { return static_cast<uint32_t>(fakeTimeUs / 1000); }
void delay(unsigned long ms) { fakeTimeUs += ms * 1000; }
void delayMicroseconds(unsigned int us) { fakeTimeUs += us; }

struct FakeSerial {
  std::string input;
  std::string output;
  int available() { return input.size(); }
  int read() {
    if (input.empty()) return -1;
    unsigned char result = input[0];
    input.erase(0, 1);
    return result;
  }
  size_t write(const uint8_t* data, size_t size) { output.append(reinterpret_cast<const char*>(data), size); return size; }
  void write(char c) { output += c; }
  void print(char c) { output += c; }
  void print(const char *s) { output += s; }
  void println(char c) { output += c; output += '\n'; }
  void println(const char *s) { output += s; output += '\n'; }
  void printf(const char *format, ...) {
    char buffer[4096];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    assert(length >= 0 && static_cast<size_t>(length) < sizeof(buffer));
    output += buffer;
  }
} Serial;
FakeSerial HardwareDebugSerial;

#include "fake_pn532_reader.h"
#include "sensor_test_state.h"
FakePn532Reader pn532;
enum GainMode { GAIN_CONTACT, GAIN_NEAR, GAIN_FAR, GAIN_DIAGNOSTIC_DEFAULT };

#include "low_level.inc"
#include "diagnostics.inc"

static void command(const std::string &text) {
  Serial.input += text + "\n";
  while (!Serial.input.empty()) RfidDiagnosticLoop();
}

int main() {
  RfidDiagnosticSetup();
  assert(diagReady && !diagRunning);
  RfidDiagnosticLoop();
  assert(pn532.uidCalls == 0);

  command("gain 18");
  assert(!diagAutoGain && pn532.commands.back()[2] == 0x09);
  pn532.commands.clear();
  pn532.uidResults = {true, false};
  command("scan 2 0");
  RfidDiagnosticLoop();
  assert(!diagRunning && diagCount == 2 && diagSuccesses == 1 && diagUidSuccesses == 1);
  assert(pn532.commands.empty()); // Fixed gain never secretly resets RF between attempts.

  command("gain auto");
  pn532.commands.clear();
  pn532.uidResults.clear();
  unsigned int beforeUid = pn532.uidCalls;
  command("scan 1 0");
  assert(pn532.uidCalls - beforeUid == 3); // Each gain is tried once after validated no-target responses.
  assert(pn532.commands.size() == 4);
  assert(pn532.commands[0][2] == 0x09 && pn532.commands[1][2] == 0x49);
  assert(pn532.commands[2][1] == 1 && pn532.commands[2][2] == 0);
  assert(pn532.commands[3][1] == 1 && pn532.commands[3][2] == 1);

  command("gain 38");
  assert(pn532.commands.back()[2] == 0x59);
  pn532.uidResults = {true}; pn532.readResults = {false};
  command("scan 1 0");
  assert(diagUidSuccesses == 1 && diagSuccesses == 0);
  assert(diagStages[1].operation == RFID_DIAG_READ && diagStages[1].data_length == 0);

  command("timeout 17"); command("retries 0");
  assert(rfid_diag_timeout_ms == 17 && pn532.lastRetries == 0);
  command("scan 1 0");
  assert(pn532.lastTimeout == 17);
  unsigned int beforeRetries = pn532.retryCalls;
  command("retries 255"); command("timeout 0"); command("timeout 251"); command("gain 28"); command("scan 1001");
  assert(pn532.retryCalls == beforeRetries && rfid_diag_timeout_ms == 17 && !diagRunning);

  beforeUid = pn532.uidCalls;
  command("scan 1000 0\nstop");
  assert(!diagRunning && diagCount == 0 && pn532.uidCalls == beforeUid);
  command("scan 2 10000");
  int gainBefore = RfidDiagnosticSoftwareGainDb();
  command("gain 18");
  assert(RfidDiagnosticSoftwareGainDb() == gainBefore && diagRunning);
  command("stop");
  command(std::string(120, 'x'));
  command("status");
  assert(!diagCommandOverflow && diagCommandLength == 0);

  pn532.ackResults = {false};
  command("gain 23");
  assert(!diagStages[0].ok && RfidDiagnosticSoftwareGainDb() == 38 && !rfid_gain_known);
  assert(diagStages[0].operation == RFID_DIAG_GAIN);

  command("reset");
  assert(diagReady && rfid_gain_known && !rfid_recovery_required);
  fakeTimeUs = UINT32_MAX - 50ULL;
  pn532.uidResults = {true}; pn532.readResults = {true};
  command("scan 1 0");
  assert(diagSuccesses == 1 && diagStages[0].elapsed_us == 100 && diagStages[1].elapsed_us == 50);
  assert(diagStages[1].started_us < diagStages[0].started_us);

  pn532.uidLengthResult = 10; pn532.uidResults = {true};
  command("scan 1 0");
  assert(diagSuccesses == 1 && diagStages[0].data_length == 10);
  assert(diagStages[0].data[9] == '0');

  DiagnosticClearStages();
  for (int i = 0; i < 21; ++i) RfidDiagnosticRecord(RFID_DIAG_UID, 23, micros(), false, nullptr, 0);
  assert(diagStageCount == 20 && diagStageOverflow);
  std::cout << Serial.output;
}
