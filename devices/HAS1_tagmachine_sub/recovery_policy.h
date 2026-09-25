#ifndef HAS1_TAGMACHINE_RECOVERY_POLICY_H
#define HAS1_TAGMACHINE_RECOVERY_POLICY_H

#include <stdint.h>

// PN532 initialization retry policy.  This header deliberately has no Arduino
// dependencies so the rollover and backoff behavior can be tested on a host.
class RfidRecoveryPolicy {
public:
    static const uint32_t kInitialRetryMs = 1000;
    static const uint32_t kMaxRetryMs = 10000;

    RfidRecoveryPolicy()
        : ready_(false), recovery_ack_pending_(false), failure_count_(0),
          next_attempt_ms_(0) {}

    void request(uint32_t now, bool recoveryAckRequired) {
        ready_ = false;
        recovery_ack_pending_ = recovery_ack_pending_ || recoveryAckRequired;
        failure_count_ = 0;
        next_attempt_ms_ = now;
    }

    bool attemptDue(uint32_t now) const {
        return !ready_ && deadlineReached(now, next_attempt_ms_);
    }

    void recordFailure(uint32_t now) {
        ready_ = false;
        if (failure_count_ < UINT8_MAX) ++failure_count_;
        next_attempt_ms_ = now + retryDelayMs(failure_count_);
    }

    void recordSuccess() {
        ready_ = true;
        failure_count_ = 0;
    }

    bool ready() const { return ready_; }
    uint8_t failureCount() const { return failure_count_; }
    uint32_t nextAttemptMs() const { return next_attempt_ms_; }
    bool recoveryAckPending() const { return recovery_ack_pending_; }

    bool takeRecoveryAck() {
        const bool pending = recovery_ack_pending_;
        recovery_ack_pending_ = false;
        return pending;
    }

    static bool deadlineReached(uint32_t now, uint32_t deadline) {
        // Signed subtraction is the conventional millis() rollover-safe test.
        return static_cast<int32_t>(now - deadline) >= 0;
    }

    static uint32_t retryDelayMs(uint8_t failureCount) {
        if (failureCount == 0) return 0;

        uint32_t delayMs = kInitialRetryMs;
        for (uint8_t i = 1; i < failureCount && delayMs < kMaxRetryMs; ++i) {
            if (delayMs > kMaxRetryMs / 2) return kMaxRetryMs;
            delayMs *= 2;
        }
        return delayMs > kMaxRetryMs ? kMaxRetryMs : delayMs;
    }

private:
    bool ready_;
    bool recovery_ack_pending_;
    uint8_t failure_count_;
    uint32_t next_attempt_ms_;
};

#endif
