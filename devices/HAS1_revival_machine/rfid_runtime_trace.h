#ifndef REVIVAL_RFID_RUNTIME_TRACE_H
#define REVIVAL_RFID_RUNTIME_TRACE_H

#include <stdint.h>

enum RfidTraceOperation { RFID_TRACE_GAIN, RFID_TRACE_UID, RFID_TRACE_PAGE7,
                          RFID_TRACE_RF_OFF, RFID_TRACE_RF_ON };
enum RfidTraceLoopPhase { RFID_TRACE_TELNET, RFID_TRACE_TIMER, RFID_TRACE_NEO,
                          RFID_TRACE_GAME };

void RfidTraceStartup(int firmware, const char *gameState, const char *deviceState);
void RfidTraceLoopBegin();
void RfidTraceLoopStage(int phase, uint32_t started);
void RfidTraceScanBegin(const char *context);
void RfidTraceScanEnd(bool ok, const uint8_t *data);
void RfidTraceRecord(int operation, uint8_t requestedGainCfg, uint32_t started,
                     bool ok, const uint8_t *data);
void RfidTraceLoopEnd(const char *gameState, const char *deviceState);

#endif
