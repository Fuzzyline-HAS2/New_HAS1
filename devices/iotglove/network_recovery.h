#pragma once
#include <stdint.h>

namespace iotglove {
enum class NetworkPhase : uint8_t { Starting, Idle, Connecting, Backoff, Http, Ota };
struct NetworkDiagnostics {
  NetworkPhase phase = NetworkPhase::Starting;
  uint32_t sequence = 0, startedAtMs = 0, progressAtMs = 0, completedAtMs = 0;
  uint32_t deadlineMs = 0, idleDeadlineMs = 0;
  uint32_t lastHttpElapsedMs = 0, lastHttpBytes = 0;
  uint32_t primaryTransportFailures = 0, forcedReconnects = 0, reconnectCooldownMs = 0;
  uint32_t workerStackHighWater = 0, heapFree = 0, heapLargest = 0;
  int lastHttpStatus = 0;
  uint8_t lastHttpError = 0, lastHttpPhase = 0;
  uint16_t wifiDisconnectReason = 0;
};

inline bool networkWorkerStalled(const NetworkDiagnostics& d, uint32_t now) {
  // Legacy OTA owns its timeouts. Without a progress hook, the worker watchdog
  // cannot distinguish a healthy flash/download from a stalled OTA operation.
  if (d.phase == NetworkPhase::Ota) return false;
  return (d.deadlineMs && uint32_t(now - d.startedAtMs) >= d.deadlineMs) ||
      (d.idleDeadlineMs && uint32_t(now - d.progressAtMs) >= d.idleDeadlineMs);
}

// Owned by the network worker. Only its periodic ReceiveMine observation is
// supplied here: side requests must neither inflate nor conceal poll failures.
class NetworkRecoveryPolicy {
 public:
  static constexpr uint32_t kFailureSpanMs = 15000;
  static constexpr uint32_t kHealthyResetMs = 60000;
  static constexpr uint32_t kMaxRetryAfterMs = 120000;

  void observePrimary(uint32_t now, bool transportFailure, bool validSnapshot) {
    if (transportFailure) {
      healthy_ = false;
      if (!failures_) firstFailureAt_ = now;
      if (failures_ != UINT32_MAX) ++failures_;
      return;
    }
    failures_ = 0;  // An HTTP error/invalid JSON still proves a responding path.
    if (!validSnapshot) { healthy_ = false; return; }
    if (!healthy_) { healthy_ = true; healthySince_ = now; }
    if (uint32_t(now - healthySince_) >= kHealthyResetMs) {
      recoveryLevel_ = 0;
      cooling_ = false;
    }
  }
  bool shouldReconnect(uint32_t now) const {
    return failures_ >= 3 && uint32_t(now - firstFailureAt_) >= kFailureSpanMs &&
        (!cooling_ || uint32_t(now - lastReconnectAt_) >= cooldownMs_);
  }
  void reconnected(uint32_t now, uint32_t entropy) {
    const uint32_t base = recoveryLevel_ == 0 ? 30000 : recoveryLevel_ == 1 ? 60000 : 120000;
    cooldownMs_ = base + entropy % 5001U;
    if (recoveryLevel_ < 2) ++recoveryLevel_;
    if (reconnects_ != UINT32_MAX) ++reconnects_;
    lastReconnectAt_ = now; cooling_ = true;
    failures_ = 0; healthy_ = false;
  }
  void disconnected() { failures_ = 0; healthy_ = false; }
  void defer(uint32_t now, uint32_t retryAfterMs) {
    if (!retryAfterMs) return;
    if (retryAfterMs > kMaxRetryAfterMs) retryAfterMs = kMaxRetryAfterMs;
    const uint32_t remaining = deferred(now) ? deferMs_ - uint32_t(now - deferAt_) : 0;
    if (retryAfterMs >= remaining) { deferAt_ = now; deferMs_ = retryAfterMs; }
  }
  bool deferred(uint32_t now) {
    if (deferMs_ && uint32_t(now - deferAt_) >= deferMs_) deferMs_ = 0;
    return deferMs_ != 0;
  }
  uint32_t failures() const { return failures_; }
  uint32_t reconnects() const { return reconnects_; }
  uint32_t cooldownMs() const { return cooling_ ? cooldownMs_ : 0; }
 private:
  uint32_t failures_ = 0, firstFailureAt_ = 0, reconnects_ = 0;
  uint32_t lastReconnectAt_ = 0, cooldownMs_ = 0, healthySince_ = 0;
  uint32_t deferAt_ = 0, deferMs_ = 0;
  uint8_t recoveryLevel_ = 0;
  bool cooling_ = false, healthy_ = false;
};
}  // namespace iotglove
