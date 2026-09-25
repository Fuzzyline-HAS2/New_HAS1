#include "HAS1_revival_machine.h"

#if REVIVAL_RFID_DIAGNOSTICS
#include <stdlib.h>
#include <string.h>

// USB-only bench mode. No scan runs until explicitly requested. These fixed-size
// buffers avoid allocation/printing inside the measured PN532 calls.
uint16_t rfid_diag_timeout_ms = RFID_DETECT_TIMEOUT_MS;
uint8_t rfid_diag_retries = RFID_ACTIVATION_RETRIES;
uint32_t rfid_diag_chip_firmware = 0;
bool rfid_diag_sam_ok = false;
bool rfid_diag_retries_ok = false;
bool rfid_diag_gain_ok = false;
static RfidDiagnosticStage diagStages[20];
static uint8_t diagStageCount = 0;
static bool diagStageOverflow = false;
static bool diagReady = false;
static bool diagAutoGain = true;
static bool diagRunning = false;
static int diagFixedGainDb = 23;
static uint32_t diagScanId = 0;
static unsigned int diagRequested = 0;
static unsigned int diagCount = 0;
static unsigned int diagSuccesses = 0;
static unsigned int diagUidSuccesses = 0;
static unsigned int diagGxpxSuccesses = 0;
static uint32_t diagIntervalMs = RFID_DEBOUNCE_MS;
static uint32_t diagNextTrialMs = 0;
static char diagCommand[96];
static uint8_t diagCommandLength = 0;
static bool diagCommandOverflow = false;

static void DiagnosticClearStages()
{
  diagStageCount = 0;
  diagStageOverflow = false;
}

void RfidDiagnosticRecord(int operation, int gainDb, uint32_t started,
                          bool ok, const uint8_t *data, uint8_t length)
{
  uint32_t elapsed = micros() - started;
  if (diagStageCount == sizeof(diagStages) / sizeof(diagStages[0]))
  {
    diagStageOverflow = true;
    return;
  }
  RfidDiagnosticStage &stage = diagStages[diagStageCount++];
  stage.started_us = started;
  stage.elapsed_us = elapsed;
  stage.gain_db = gainDb;
  stage.operation = operation;
  stage.ok = ok;
  stage.data_length = (data && length < sizeof(stage.data)) ? length :
                      (data ? sizeof(stage.data) : 0);
  if (stage.data_length) memcpy(stage.data, data, stage.data_length);
}

static void DiagnosticPrintHex(const uint8_t *data, uint8_t length)
{
  static const char hex[] = "0123456789ABCDEF";
  for (uint8_t i = 0; i < length; ++i)
  {
    Serial.write(hex[data[i] >> 4]);
    Serial.write(hex[data[i] & 15]);
  }
}

static void DiagnosticPrintStages()
{
  static const char *names[] = {"gain", "uid", "read", "rf_off", "rf_on"};
  Serial.print(",\"hardware_gain_confirmed\":false,\"stages\":[");
  for (uint8_t i = 0; i < diagStageCount; ++i)
  {
    const RfidDiagnosticStage &stage = diagStages[i];
    if (i) Serial.print(',');
    Serial.printf("{\"op\":\"%s\",\"gain_db\":%d,\"started_us\":%lu,\"elapsed_us\":%lu,\"ok\":%s",
                  names[stage.operation], stage.gain_db,
                  (unsigned long)stage.started_us, (unsigned long)stage.elapsed_us,
                  stage.ok ? "true" : "false");
    if (stage.operation == RFID_DIAG_GAIN || stage.operation == RFID_DIAG_RF_OFF ||
        stage.operation == RFID_DIAG_RF_ON)
      Serial.print(",\"result_kind\":\"validated_response\"");
    if (stage.operation == RFID_DIAG_UID || stage.operation == RFID_DIAG_READ)
    {
      Serial.print(stage.operation == RFID_DIAG_UID ? ",\"uid_hex\":\"" : ",\"payload_hex\":\"");
      DiagnosticPrintHex(stage.data, stage.data_length);
      Serial.print('"');
    }
    Serial.print('}');
  }
  Serial.printf("],\"stages_overflow\":%s", diagStageOverflow ? "true" : "false");
}

static void DiagnosticPrintStatusFields()
{
  Serial.printf(",\"pn532_ready\":%s,\"running\":%s,\"mode\":\"%s\",\"gain_db\":%d,\"software_gain_db\":%d,\"timeout_ms\":%u,\"retries\":%u,\"scan_id\":%lu",
                (diagReady && !rfid_recovery_required && rfid_gain_known) ? "true" : "false", diagRunning ? "true" : "false",
                diagAutoGain ? "auto" : "fixed", diagFixedGainDb,
                RfidDiagnosticSoftwareGainDb(), rfid_diag_timeout_ms, rfid_diag_retries,
                (unsigned long)diagScanId);
  Serial.printf(",\"recovery_required\":%s,\"recovery_locked\":%s,\"recovery_attempts\":%u,\"gain_known\":%s,\"last_outcome\":%u,\"transport_fault\":\"%s\",\"transport_phase\":\"%s\",\"spi_status\":%u",
                rfid_recovery_required ? "true" : "false", rfid_recovery_locked ? "true" : "false",
                rfid_recovery_attempts, rfid_gain_known ? "true" : "false", (unsigned int)rfid_last_outcome,
                pn532.faultName(), pn532.phaseName(), pn532.lastStatus());

}

static void DiagnosticError(const char *command, const char *reason)
{
  Serial.printf("[RFID_DIAG] {\"protocol\":1,\"event\":\"error\",\"cmd\":\"%s\",\"reason\":\"%s\"}\n",
                command, reason);
}

static void DiagnosticAck(const char *command, bool ok)
{
  Serial.printf("[RFID_DIAG] {\"protocol\":1,\"event\":\"ack\",\"cmd\":\"%s\",\"ok\":%s",
                command, ok ? "true" : "false");
  DiagnosticPrintStatusFields();
  DiagnosticPrintStages();
  Serial.println('}');
}

static void DiagnosticDone(const char *reason)
{
  diagRunning = false;
  Serial.printf("[RFID_DIAG] {\"protocol\":1,\"event\":\"done\",\"scan_id\":%lu,\"requested\":%u,\"count\":%u,\"successes\":%u,\"uid_successes\":%u,\"gxpx_successes\":%u,\"reason\":\"%s\"}\n",
                (unsigned long)diagScanId, diagRequested, diagCount, diagSuccesses,
                diagUidSuccesses, diagGxpxSuccesses, reason);
}

static bool DiagnosticNumber(const char *text, unsigned long minimum,
                             unsigned long maximum, unsigned long *value)
{
  if (!text || !*text) return false;
  // Reject signs, partial numbers and overlong values before strtoul.
  if (strlen(text) > 5) return false;
  for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return false;
  *value = strtoul(text, nullptr, 10);
  return *value >= minimum && *value <= maximum;
}

static void DiagnosticCommand(char *line)
{
  char *save = nullptr;
  char *command = strtok_r(line, " \t", &save);
  if (!command) return;
  char *first = strtok_r(nullptr, " \t", &save);
  char *second = strtok_r(nullptr, " \t", &save);
  char *extra = strtok_r(nullptr, " \t", &save);
  // Never echo untrusted input into JSON; only known command literals below.
  if (extra) { DiagnosticError("input", "too_many_arguments"); return; }
  if (!strcmp(command, "help") && !first)
  {
    Serial.println("[RFID_DIAG] {\"protocol\":1,\"event\":\"help\",\"commands\":[\"status\",\"gain auto|18|23|33|38\",\"timeout 1..250\",\"retries 0..50\",\"scan 1..1000 [interval_ms 0..10000]\",\"stop\",\"reset\"],\"polling\":\"continuous; fixed gain does not reset RF between trials; auto resets RF only after clean no-target sweep\",\"stop\":\"between trials only\"}");
    return;
  }
  if (!strcmp(command, "status") && !first)
  {
    Serial.print("[RFID_DIAG] {\"protocol\":1,\"event\":\"status\"");
    DiagnosticPrintStatusFields();
    Serial.println('}');
    return;
  }
  if (!strcmp(command, "stop") && !first)
  {
    bool wasRunning = diagRunning;
    diagRunning = false;
    DiagnosticClearStages();
    DiagnosticAck("stop", true);
    if (wasRunning) DiagnosticDone("stopped");
    return;
  }
  if (diagRunning) { DiagnosticError("input", "scan_running"); return; }
  DiagnosticClearStages();
  if (!strcmp(command, "reset") && !first)
  {
    // Reinitializes PN532 host configuration; not a power cycle or card re-presentation.
    diagReady = RfidDiagnosticHardwareInit();
    diagAutoGain = true;
    diagFixedGainDb = 23;
    DiagnosticAck("reset", diagReady);
    return;
  }
  if (!strcmp(command, "gain") && first && !second)
  {
    if (!strcmp(first, "auto"))
    {
      diagAutoGain = true;
      diagFixedGainDb = 23;
      // Start baseline auto at NEAR, matching RfidInit().
      bool ok = RfidDiagnosticSetGainDb(23);
      DiagnosticAck("gain", ok);
      return;
    }
    unsigned long value;
    if (!DiagnosticNumber(first, 18, 38, &value) ||
        (value != 18 && value != 23 && value != 33 && value != 38))
    { DiagnosticError("gain", "expected_auto_18_23_33_38"); return; }
    diagAutoGain = false;
    diagFixedGainDb = value;
    bool ok = RfidDiagnosticSetGainDb(value);
    DiagnosticAck("gain", ok);
    return;
  }
  if (!strcmp(command, "timeout") && first && !second)
  {
    unsigned long value;
    if (!DiagnosticNumber(first, 1, 250, &value))
    { DiagnosticError("timeout", "expected_1_to_250_ms"); return; }
    rfid_diag_timeout_ms = value;
    DiagnosticAck("timeout", true);
    return;
  }
  if (!strcmp(command, "retries") && first && !second)
  {
    unsigned long value;
    if (!DiagnosticNumber(first, 0, 50, &value))
    { DiagnosticError("retries", "expected_0_to_50"); return; }
    bool ok = RfidDiagnosticSetRetries(value);
    if (ok) rfid_diag_retries = value;
    DiagnosticAck("retries", ok);
    return;
  }
  if (!strcmp(command, "scan") && first)
  {
    unsigned long count, interval = RFID_DEBOUNCE_MS;
    if (!DiagnosticNumber(first, 1, 1000, &count) ||
        (second && !DiagnosticNumber(second, 0, 10000, &interval)))
    { DiagnosticError("scan", "expected_count_1_to_1000_interval_0_to_10000"); return; }
    if (!diagReady || rfid_recovery_required || !rfid_gain_known) { DiagnosticError("scan", "pn532_not_ready"); return; }
    ++diagScanId;
    diagRequested = count;
    diagCount = diagSuccesses = diagUidSuccesses = diagGxpxSuccesses = 0;
    diagIntervalMs = interval;
    diagNextTrialMs = millis();
    diagRunning = true;
    Serial.printf("[RFID_DIAG] {\"protocol\":1,\"event\":\"ack\",\"cmd\":\"scan\",\"ok\":true,\"scan_id\":%lu,\"requested\":%u,\"interval_ms\":%lu}\n",
                  (unsigned long)diagScanId, diagRequested, (unsigned long)diagIntervalMs);
    return;
  }
  DiagnosticError("input", "unknown_command_or_arguments");
}

void RfidDiagnosticSetup()
{
  DiagnosticClearStages();
  diagReady = RfidDiagnosticHardwareInit();
  Serial.printf("[RFID_DIAG] {\"protocol\":1,\"event\":\"ready\",\"firmware\":%d,\"chip_firmware\":%lu,\"sam_ok\":%s,\"retries_ok\":%s,\"gain_ack_ok\":%s",
                FIRMWARE_VER, (unsigned long)rfid_diag_chip_firmware,
                rfid_diag_sam_ok ? "true" : "false", rfid_diag_retries_ok ? "true" : "false",
                rfid_diag_gain_ok ? "true" : "false");
  DiagnosticPrintStatusFields();
  DiagnosticPrintStages();
  Serial.println('}');
}

void RfidDiagnosticLoop()
{
  // Bound input work per loop, including when a sender never terminates a line.
  for (unsigned int budget = 0; budget < sizeof(diagCommand) && Serial.available(); ++budget)
  {
    int input = Serial.read();
    if (input < 0) break;
    if (input == '\r') continue;
    if (input == '\n')
    {
      if (diagCommandOverflow) DiagnosticError("input", "line_too_long_or_invalid");
      else { diagCommand[diagCommandLength] = 0; DiagnosticCommand(diagCommand); }
      diagCommandLength = 0;
      diagCommandOverflow = false;
    }
    else if (input < 32 || input > 126 || diagCommandLength == sizeof(diagCommand) - 1)
      diagCommandOverflow = true;
    else if (!diagCommandOverflow) diagCommand[diagCommandLength++] = (char)input;
  }
  if (!diagRunning || (int32_t)(millis() - diagNextTrialMs) < 0)
  { delay(1); return; }

  uint8_t data[32] = {};
  DiagnosticClearStages();
  uint32_t started = micros();
  bool ok = RfidDiagnosticRead(diagAutoGain, data);
  uint32_t elapsed = micros() - started;
  bool uidOk = false;
  for (uint8_t i = 0; i < diagStageCount; ++i)
    if (diagStages[i].operation == RFID_DIAG_UID && diagStages[i].ok) uidOk = true;
  bool valid = ok && data[0] == 'G' && data[1] >= '0' && data[1] <= '9' &&
               data[2] == 'P' && data[3] >= '0' && data[3] <= '9';
  ++diagCount;
  if (ok) ++diagSuccesses;
  if (uidOk) ++diagUidSuccesses;
  if (valid) ++diagGxpxSuccesses;
  Serial.printf("[RFID_DIAG] {\"protocol\":1,\"event\":\"trial\",\"scan_id\":%lu,\"id\":%u,\"started_us\":%lu,\"elapsed_us\":%lu,\"mode\":\"%s\",\"software_gain_db\":%d,\"ok\":%s,\"uid_ok\":%s,\"valid_gxpx\":%s,\"payload_hex\":\"",
                (unsigned long)diagScanId, diagCount, (unsigned long)started,
                (unsigned long)elapsed, diagAutoGain ? "auto" : "fixed",
                RfidDiagnosticSoftwareGainDb(), ok ? "true" : "false",
                uidOk ? "true" : "false", valid ? "true" : "false");
  if (ok) DiagnosticPrintHex(data, 4);
  Serial.print('"');
  DiagnosticPrintStages();
  Serial.println('}');
  if (diagCount >= diagRequested) DiagnosticDone("complete");
  // USB logging time is outside the measurement, and interval is a minimum gap
  // after that logging. This is continuous polling, not independent tag visits.
  diagNextTrialMs = millis() + diagIntervalMs;
  delay(1);
}
#endif
