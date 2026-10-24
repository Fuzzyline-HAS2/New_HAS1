#pragma once
#include <stdint.h>
#include "network_recovery.h"

namespace iotglove {
// Retained across our requested SW restarts and watchdog resets. No flash writes.
// GWR3 invalidates the former three-attempt budget semantics after an upgrade.
struct NetworkRestartHistory {
  uint32_t magic;
  uint32_t attempts;
  bool requested;
};
struct NetworkRestartDecision {
  uint8_t cause = 0;  // Same recovery cause contract: 2 worker stall, 3 Wi-Fi down.
  bool waitingStarted = false, cancelled = false, restored = false, restart = false;
  uint32_t waitMs = 0, remainingMs = 0, elapsedMs = 0, previousAttempts = 0;
};
class NetworkRestartPolicy {
 public:
  static constexpr uint32_t kMagic = 0x47575233U;
  static constexpr uint32_t kStableMs = 10U * 60U * 1000U;
  static constexpr uint32_t kMaxProgressGapMs = 180000U;
  static void boot(NetworkRestartHistory& history, bool softwareReset, bool watchdogReset) {
    if (history.magic != kMagic || (!softwareReset && !watchdogReset) ||
        (softwareReset && !history.requested)) {
      history = {kMagic, 0, false}; return;
    }
    if (watchdogReset && !history.requested && history.attempts != UINT32_MAX)
      ++history.attempts;
    history.requested = false;
  }
  static uint32_t backoffMs(uint32_t attempts) {
    return attempts == 0 ? 0 : attempts == 1 ? 300000U : attempts == 2 ? 900000U : 1800000U;
  }
  static uint8_t diagnosticAttempt(uint32_t attempts) {
    return attempts > UINT8_MAX ? UINT8_MAX : static_cast<uint8_t>(attempts);
  }
  // Testable production monitor: logging latches do not participate in retry
  // admission. A reported persistent fault is reevaluated on EVERY main pass.
  NetworkRestartDecision poll(NetworkRestartHistory& history, const NetworkDiagnostics& d,
                              uint32_t now, uint32_t entropy, bool otaBusy = false) {
    NetworkRestartDecision out;
    const bool ota = otaBusy || d.phase == NetworkPhase::Ota;
    const bool stalled = !ota && networkWorkerStalled(d, now);
    if (observeHealthy(now, d.sequence, !ota && d.wifiConnected && !stalled) && history.attempts) {
      out.restored = true; out.previousAttempts = history.attempts;
      history = {kMagic, 0, false};
    }
    const bool linkDown = !ota && wifiLinkDownTooLong(d, now);
    out.cause = stalled ? 2 : linkDown ? 3 : 0;
    if (!out.cause) {
      if (pending_) {
        out.cancelled = true; out.cause = cause_; out.elapsedMs = now - outageSince_;
      }
      pending_ = false; cause_ = 0; return out;
    }
    uint32_t age = now-d.wifiDownSinceMs;
    uint32_t threshold = d.wifiWasUp ? kWifiLostRestartMs : kWifiNeverUpRestartMs;
    if (stalled) {
      const bool totalExpired = d.deadlineMs && uint32_t(now-d.startedAtMs) >= d.deadlineMs;
      age = totalExpired ? now-d.startedAtMs : now-d.progressAtMs;
      threshold = totalExpired ? d.deadlineMs : d.idleDeadlineMs;
      if (linkDown && uint32_t(now-d.wifiDownSinceMs) > age) age = now-d.wifiDownSinceMs;
    }
    if (!pending_) {
      pending_ = true; outageSince_ = now-age;
      const uint32_t backoff = backoffMs(history.attempts);
      waitMs_ = (backoff > threshold ? backoff : threshold) +
          (history.attempts ? entropy % 5001U : 0U);
      out.waitingStarted = true;
    } else if (age > uint32_t(now-outageSince_)) {
      // Fault-kind changes must not restart the clock forever. Preserve the
      // earliest observed continuous outage even when worker/link causes swap.
      outageSince_ = now-age;
    }
    cause_ = out.cause;
    out.waitMs = waitMs_; out.elapsedMs = now-outageSince_;
    out.remainingMs = remainingMs(now);
    if (!out.remainingMs) {
      if (history.attempts != UINT32_MAX) ++history.attempts;
      history.requested = true; out.restart = true;
      pending_ = false; cause_ = 0;
    }
    return out;
  }
  uint32_t remainingMs(uint32_t now) const {
    if (!pending_) return 0;
    const uint32_t elapsed = now-outageSince_;
    return elapsed < waitMs_ ? waitMs_-elapsed : 0;
  }
 private:
  bool pending_ = false, tracking_ = false;
  uint8_t cause_ = 0;
  uint32_t outageSince_ = 0, waitMs_ = 0;
  uint32_t since_ = 0, advancedAt_ = 0, sequence_ = 0;
  bool observeHealthy(uint32_t now, uint32_t sequence, bool linkAndWorkerHealthy) {
    if (!linkAndWorkerHealthy) { tracking_ = false; return false; }
    if (!tracking_) {
      tracking_ = true; since_ = advancedAt_ = now; sequence_ = sequence; return false;
    }
    if (sequence != sequence_) { sequence_ = sequence; advancedAt_ = now; }
    if (uint32_t(now-advancedAt_) > kMaxProgressGapMs) { tracking_ = false; return false; }
    if (uint32_t(now-since_) < kStableMs) return false;
    since_ = now; return true;
  }
};
}  // namespace iotglove
