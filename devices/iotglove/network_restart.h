#pragma once
#include <stdint.h>

namespace iotglove {

// Stored in RTC RAM, never written to flash. This limits recovery-induced boot
// loops; a cold boot clears it. Independent hardware/power failures can still reset.
struct NetworkRestartHistory {
  uint32_t magic;
  uint32_t attempts;
  bool requested;
};

class NetworkRestartPolicy {
 public:
  static constexpr uint32_t kMagic = 0x47575232U;
  static constexpr uint32_t kMaxRestarts = 3;
  static constexpr uint32_t kStableMs = 10U * 60U * 1000U;
  static constexpr uint32_t kMaxProgressGapMs = 180000U;

  static void boot(NetworkRestartHistory& history, bool softwareReset, bool watchdogReset) {
    if (history.magic != kMagic || (!softwareReset && !watchdogReset) ||
        (softwareReset && !history.requested)) {
      history = {kMagic, 0, false};
      return;
    }
    // Explicit worker-stall restarts are counted before ESP.restart(). A main
    // loop TWDT panic cannot do that, so account for it on the following boot.
    if (watchdogReset && !history.requested && history.attempts < kMaxRestarts)
      ++history.attempts;
    history.requested = false;
  }

  static bool allowed(const NetworkRestartHistory& history) {
    return history.attempts < kMaxRestarts;
  }

  static bool request(NetworkRestartHistory& history) {
    if (!allowed(history)) return false;
    ++history.attempts;
    history.requested = true;
    return true;
  }

  // Server success is deliberately irrelevant: completed failed requests and
  // reconnect attempts are healthy worker progress too. A frozen sequence never
  // earns a fresh reboot budget simply because the main loop remains alive.
  bool observe(uint32_t now, uint32_t sequence, bool stalled) {
    if (stalled) { tracking_ = false; return false; }
    if (!tracking_) {
      tracking_ = true; since_ = advancedAt_ = now; sequence_ = sequence;
      return false;
    }
    if (sequence != sequence_) { sequence_ = sequence; advancedAt_ = now; }
    if (uint32_t(now - advancedAt_) > kMaxProgressGapMs) {
      tracking_ = false;
      return false;
    }
    if (uint32_t(now - since_) < kStableMs) return false;
    since_ = now;
    return true;
  }

 private:
  bool tracking_ = false;
  uint32_t since_ = 0, advancedAt_ = 0, sequence_ = 0;
};

}  // namespace iotglove
