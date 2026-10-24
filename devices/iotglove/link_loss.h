#pragma once
#include <stdint.h>

namespace iotglove {

// Why the main loop requested a recovery restart. None marks a link loss whose
// context was captured but which had not yet caused a restart.
enum class LinkLossCause : uint8_t { None, WifiDown, WorkerStall };

inline const char* linkLossCauseName(LinkLossCause cause) {
  switch (cause) {
    case LinkLossCause::WifiDown: return "wifi_down";
    case LinkLossCause::WorkerStall: return "worker_stall";
    default: return "none";
  }
}

// Stored in RTC RAM like NetworkRestartHistory: it survives software, watchdog
// and brownout resets so the next boot can print what the link looked like when
// it was lost. A cold boot (battery removed) clears it. The worker's disconnect
// event fills the loss moment; the main loop adds game/output context and, when
// a restart follows, the cause and the uptime of the restart request.
struct LinkLossRecord {
  uint32_t magic;
  uint8_t cause;           // LinkLossCause
  uint8_t stateKnown;      // phase/role/deviceState came from a valid snapshot
  uint8_t phase, role, deviceState;
  uint8_t motor, lit;      // outputs when the loss was noticed
  uint8_t restartAttempt;  // NetworkRestartHistory.attempts after the request
  uint16_t reason;         // Wi-Fi STA disconnect reason from the event
  int8_t rssi;             // last RSSI sampled while the link was still up
  uint8_t padding;
  uint32_t lostAtMs;       // uptime at the disconnect event
  uint32_t noticedAtMs;    // uptime when the main loop captured the context
  uint32_t restartAtMs;    // uptime when the restart was requested; 0 while pending
  uint32_t motorAgeMs;     // ms since the motor was last ON when noticed; UINT32_MAX = never
  uint32_t connectFailures;
  uint32_t heapFree;
  uint16_t batteryAdcMv;   // raw ADC millivolts on the battery pin, 0 when unread
  uint16_t batteryMv;      // calibrated battery millivolts, 0 when not configured
};

constexpr uint32_t kLinkLossMagic = 0x4C4C4B31U;  // "LLK1"
constexpr uint32_t kLinkLossMotorNever = 0xFFFFFFFFU;

inline bool linkLossPresent(const LinkLossRecord& record) { return record.magic == kLinkLossMagic; }
inline bool linkLossPending(const LinkLossRecord& record) {
  return linkLossPresent(record) && record.restartAtMs == 0;
}
inline void linkLossClear(LinkLossRecord& record) { record = LinkLossRecord{}; }

// Context the main loop owns at capture time. Kept separate from the record so
// the capture is a single assignment and host tests can drive it directly.
struct LinkLossContext {
  bool stateKnown = false;
  uint8_t phase = 0, role = 0, deviceState = 0;
  bool motor = false;
  uint8_t lit = 0;
  uint32_t motorAgeMs = kLinkLossMotorNever;
  uint32_t heapFree = 0;
  uint16_t batteryAdcMv = 0;
  uint16_t batteryMv = 0;
};

inline void linkLossCapture(LinkLossRecord& record, uint32_t now, uint32_t lostAtMs,
                            uint16_t reason, int8_t rssi, uint32_t connectFailures,
                            const LinkLossContext& context) {
  linkLossClear(record);
  record.magic = kLinkLossMagic;
  record.cause = static_cast<uint8_t>(LinkLossCause::None);
  record.stateKnown = context.stateKnown;
  record.phase = context.phase; record.role = context.role; record.deviceState = context.deviceState;
  record.motor = context.motor; record.lit = context.lit;
  record.reason = reason; record.rssi = rssi;
  record.lostAtMs = lostAtMs; record.noticedAtMs = now;
  record.motorAgeMs = context.motorAgeMs;
  record.connectFailures = connectFailures;
  record.heapFree = context.heapFree;
  record.batteryAdcMv = context.batteryAdcMv; record.batteryMv = context.batteryMv;
}

// Completes a pending record, or starts one from the current context when no
// link loss preceded the restart (a worker stall with the STA still associated).
inline void linkLossComplete(LinkLossRecord& record, LinkLossCause cause, uint32_t now,
                             uint32_t connectFailures, uint8_t restartAttempt,
                             const LinkLossContext& context) {
  if (!linkLossPending(record)) linkLossCapture(record, now, 0, 0, 0, connectFailures, context);
  record.cause = static_cast<uint8_t>(cause);
  record.restartAtMs = now;
  record.restartAttempt = restartAttempt;
  record.connectFailures = connectFailures;
}

}  // namespace iotglove
