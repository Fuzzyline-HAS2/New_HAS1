#ifndef HAS1_ALTAR_RECOVERY_POLICY_H
#define HAS1_ALTAR_RECOVERY_POLICY_H

#include <stdint.h>

// PN532 재초기화 재시도 정책 (HAS1_tagmachine_sub/recovery_policy.h에서 이식).
// Arduino 의존성이 없어 호스트 테스트에서 롤오버/백오프 동작을 그대로 검증할 수 있다.
//
// 제단은 리더가 죽어도 "태그 없음"과 구분하지 못해 영원히 기다리던 구조였다. 게인 명령
// 같은 ACK 필수 명령이 실패하면 request()로 재초기화를 예약하고, loop()의 RfidMaintain이
// attemptDue()일 때만 RfidInit을 다시 시도한다. 실패할수록 1s→2s→…→10s로 간격을 늘려
// 죽은 모듈에 매 tick 약 2초씩 막히는 일을 피한다(wakeup의 SAMConfig 대기 1초 + getFirmwareVersion 1초).
class RfidRecoveryPolicy {
public:
    static const uint32_t kInitialRetryMs = 1000;
    static const uint32_t kMaxRetryMs = 10000;

    RfidRecoveryPolicy() : ready_(false), failure_count_(0), next_attempt_ms_(0) {}

    void request(uint32_t now) {
        ready_ = false;
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
    uint8_t failure_count_;
    uint32_t next_attempt_ms_;
};

#endif
