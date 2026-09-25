#include "HAS1_revival_machine.h"

#if REVIVAL_RFID_RUNTIME_TRACE
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// The current loop performs at most one physical scan. Bound both capture and
// formatting storage; unexpected extra scans/stages are counted, never allocated.
static const uint32_t RFID_TRACE_SLOW_LOOP_US = 40000;
static const uint8_t RFID_TRACE_STAGE_CAPACITY = 16;
struct RfidRuntimeStage {
  uint32_t offset_us;
  uint32_t duration_us;
  uint8_t operation;
  uint8_t requested_gain_cfg;
  bool ok;
  char payload_hex[9];
};
struct RfidRuntimeLoop {
  uint32_t start_us;
  uint32_t phase_us[4];
  uint32_t scan_start_us;
  uint32_t scan_duration_us;
  uint32_t scan_end_us;
  uint32_t scan_gap_us;
  const char *context;
  bool has_scan;
  bool has_previous_scan;
  bool scan_ok;
  char payload_hex[9];
  uint8_t stage_count;
  uint32_t stages_dropped;
  RfidRuntimeStage stages[RFID_TRACE_STAGE_CAPACITY];
};
static RfidRuntimeLoop traceLoop;
static bool traceLoopOpen = false;
static bool traceScanActive = false;
static bool traceScanCapturing = false;
static bool traceHasPreviousScan = false;
static uint32_t tracePreviousScanEndUs = 0;
static uint32_t tracePreviousEmitUs = 0;
static uint32_t tracePreviousEmitBytes = 0;
static uint32_t traceDroppedRecords = 0;
static int traceFirmware = 0;
static char traceOutput[1536];
static size_t traceOutputLength = 0;
static bool traceOutputOverflow = false;

static void RfidTraceHex(const uint8_t *data, char *hex)
{
  static const char digits[] = "0123456789ABCDEF";
  hex[0] = '\0';
  if (!data) return;
  for (uint8_t i = 0; i < 4; ++i)
  {
    hex[2 * i] = digits[data[i] >> 4];
    hex[2 * i + 1] = digits[data[i] & 15];
  }
  hex[8] = '\0';
}

// States are short labels, not arbitrary JSON/server data. Bound and sanitize
// them so a malformed server value cannot break the USB JSONL record.
static void RfidTraceState(const char *value, char *output)
{
  size_t length = 0;
  if (value)
    for (; length < 23 && value[length]; ++length)
    {
      const char c = value[length];
      output[length] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-') ? c : '?';
    }
  output[length] = '\0';
}

static void RfidTraceAppend(const char *format, ...)
{
  if (traceOutputOverflow) return;
  va_list arguments;
  va_start(arguments, format);
  int count = vsnprintf(traceOutput + traceOutputLength,
                        sizeof(traceOutput) - traceOutputLength, format, arguments);
  va_end(arguments);
  if (count < 0 || (size_t)count >= sizeof(traceOutput) - traceOutputLength)
  {
    traceOutputOverflow = true;
    return;
  }
  traceOutputLength += (size_t)count;
}

static void RfidTraceEmit(uint32_t started)
{
  if (traceOutputOverflow)
  {
    ++traceDroppedRecords;
    return; // Never emit a partial JSON record.
  }
  // Bypass TelnetDebugConsole: trace output must not add TCP writes to the loop.
  size_t written = HardwareDebugSerial.write((const uint8_t *)traceOutput, traceOutputLength);
  if (written != traceOutputLength) ++traceDroppedRecords;
  tracePreviousEmitUs = (uint32_t)(micros() - started);
  tracePreviousEmitBytes = (uint32_t)written;
}

void RfidTraceStartup(int firmware, const char *gameState, const char *deviceState)
{
  const uint32_t started = micros();
  traceFirmware = firmware;
  char game[24], device[24];
  RfidTraceState(gameState, game);
  RfidTraceState(deviceState, device);
  traceOutputLength = 0;
  traceOutputOverflow = false;
  RfidTraceAppend("[RFID_TRACE] {\"v\":1,\"event\":\"startup\",\"runtime_trace\":true,\"diagnostics\":false,\"fw\":%d,\"game_state\":\"%s\",\"device_state\":\"%s\",\"uid_timeout_ms\":%u,\"rf_config_timeout_ms\":100,\"retries\":%u,\"slow_loop_us\":%lu}\n",
                  firmware, game, device, (unsigned int)RFID_DETECT_TIMEOUT_MS,
                  (unsigned int)RFID_ACTIVATION_RETRIES, (unsigned long)RFID_TRACE_SLOW_LOOP_US);
  RfidTraceEmit(started);
}

void RfidTraceLoopBegin()
{
  memset(&traceLoop, 0, sizeof(traceLoop));
  traceLoop.start_us = micros();
  traceLoopOpen = true;
  traceScanActive = false;
  traceScanCapturing = false;
}

void RfidTraceLoopStage(int phase, uint32_t started)
{
  const uint32_t elapsed = (uint32_t)(micros() - started);
  if (traceLoopOpen && phase >= 0 && phase < 4) traceLoop.phase_us[phase] += elapsed;
}

void RfidTraceScanBegin(const char *context)
{
  const uint32_t started = micros();
  traceScanActive = traceLoopOpen;
  traceScanCapturing = traceLoopOpen && !traceLoop.has_scan;
  if (!traceScanCapturing)
  {
    if (traceLoopOpen) ++traceDroppedRecords;
    return;
  }
  traceLoop.has_scan = true;
  traceLoop.context = context;
  traceLoop.scan_start_us = started;
  traceLoop.has_previous_scan = traceHasPreviousScan;
  traceLoop.scan_gap_us = (uint32_t)(started - tracePreviousScanEndUs);
}

void RfidTraceScanEnd(bool ok, const uint8_t *data)
{
  const uint32_t ended = micros();
  if (!traceScanActive) return;
  tracePreviousScanEndUs = ended;
  traceHasPreviousScan = true;
  if (traceScanCapturing)
  {
    traceLoop.scan_duration_us = (uint32_t)(ended - traceLoop.scan_start_us);
    traceLoop.scan_end_us = ended;
    traceLoop.scan_ok = ok;
    RfidTraceHex(ok ? data : nullptr, traceLoop.payload_hex);
  }
  traceScanActive = false;
  traceScanCapturing = false;
}

void RfidTraceRecord(int operation, uint8_t requestedGainCfg, uint32_t started,
                     bool ok, const uint8_t *data)
{
  const uint32_t elapsed = (uint32_t)(micros() - started);
  if (!traceScanCapturing) return; // Ignore PN532 initialization outside actual scans.
  if (traceLoop.stage_count == RFID_TRACE_STAGE_CAPACITY)
  {
    ++traceLoop.stages_dropped;
    return;
  }
  RfidRuntimeStage &stage = traceLoop.stages[traceLoop.stage_count++];
  stage.offset_us = (uint32_t)(started - traceLoop.scan_start_us);
  stage.duration_us = elapsed;
  stage.operation = (uint8_t)operation;
  stage.requested_gain_cfg = requestedGainCfg;
  stage.ok = ok;
  RfidTraceHex(ok && operation == RFID_TRACE_PAGE7 ? data : nullptr, stage.payload_hex);
}

void RfidTraceLoopEnd(const char *gameState, const char *deviceState)
{
  const uint32_t ended = micros();
  if (!traceLoopOpen) return;
  traceLoopOpen = false;
  traceScanActive = false;
  traceScanCapturing = false;
  const uint32_t loopUs = (uint32_t)(ended - traceLoop.start_us);
  if (!traceLoop.has_scan && loopUs < RFID_TRACE_SLOW_LOOP_US) return;

  char game[24], device[24];
  RfidTraceState(gameState, game);
  RfidTraceState(deviceState, device);
  traceOutputLength = 0;
  traceOutputOverflow = false;
  RfidTraceAppend("[RFID_TRACE] {\"v\":1,\"event\":\"loop\",\"runtime_trace\":true,\"fw\":%d,\"loop_start_us\":%lu,\"loop_us\":%lu,\"telnet_us\":%lu,\"timer_us\":%lu,\"neo_us\":%lu,\"rfid_game_us\":%lu,\"game_state\":\"%s\",\"device_state\":\"%s\",\"prev_emit_us\":%lu,\"prev_emit_bytes\":%lu,\"dropped\":%lu,\"stage_dropped\":%lu,\"post_read_us\":",
                  traceFirmware, (unsigned long)traceLoop.start_us, (unsigned long)loopUs,
                  (unsigned long)traceLoop.phase_us[RFID_TRACE_TELNET],
                  (unsigned long)traceLoop.phase_us[RFID_TRACE_TIMER],
                  (unsigned long)traceLoop.phase_us[RFID_TRACE_NEO],
                  (unsigned long)traceLoop.phase_us[RFID_TRACE_GAME], game, device,
                  (unsigned long)tracePreviousEmitUs, (unsigned long)tracePreviousEmitBytes,
                  (unsigned long)traceDroppedRecords, (unsigned long)traceLoop.stages_dropped);
  if (!traceLoop.has_scan)
    RfidTraceAppend("null,\"scan\":null}\n");
  else
  {
    RfidTraceAppend("%lu,\"scan\":{\"ctx\":\"%s\",\"start_us\":%lu,\"us\":%lu,\"gap_us\":",
                    (unsigned long)(uint32_t)(ended - traceLoop.scan_end_us), traceLoop.context,
                    (unsigned long)traceLoop.scan_start_us, (unsigned long)traceLoop.scan_duration_us);
    if (traceLoop.has_previous_scan)
      RfidTraceAppend("%lu", (unsigned long)traceLoop.scan_gap_us);
    else
      RfidTraceAppend("null");
    RfidTraceAppend(",\"ok\":%s,\"payload_hex\":\"%s\",\"stages\":[",
                    traceLoop.scan_ok ? "true" : "false", traceLoop.payload_hex);
    static const char *names[] = {"gain", "uid", "page7", "rf_off", "rf_on"};
    for (uint8_t i = 0; i < traceLoop.stage_count; ++i)
    {
      const RfidRuntimeStage &stage = traceLoop.stages[i];
      // [operation, requested gain byte, scan-relative start, duration, ok, first4]
      RfidTraceAppend("%s[\"%s\",%u,%lu,%lu,%u,\"%s\"]", i ? "," : "",
                      stage.operation <= RFID_TRACE_RF_ON ? names[stage.operation] : "unknown",
                      (unsigned int)stage.requested_gain_cfg, (unsigned long)stage.offset_us,
                      (unsigned long)stage.duration_us, stage.ok ? 1U : 0U, stage.payload_hex);
    }
    RfidTraceAppend("]}}\n");
  }
  // Append bounded reader-health fields inside the final JSON object.
  if (!traceOutputOverflow && traceOutputLength >= 2) {
    traceOutputLength -= 2; // Remove final } and newline only (scan object is retained).
    RfidTraceAppend(",\"reader_outcome\":%u,\"recovery_required\":%s,\"recovery_attempts\":%u,\"gain_known\":%s,\"spi_status\":%u,\"transport_fault\":\"%s\",\"transport_phase\":\"%s\"}\n",
                    (unsigned int)rfid_last_outcome, rfid_recovery_required ? "true" : "false",
                    rfid_recovery_attempts, rfid_gain_known ? "true" : "false", pn532.lastStatus(),
                    pn532.faultName(), pn532.phaseName());
  }
  RfidTraceEmit(ended);
}
#endif
