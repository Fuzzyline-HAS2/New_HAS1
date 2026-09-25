#include <assert.h>
#include <stdint.h>

#include "../recovery_policy.h"

static void test_exponential_backoff_is_bounded() {
    assert(RfidRecoveryPolicy::retryDelayMs(0) == 0);
    assert(RfidRecoveryPolicy::retryDelayMs(1) == 1000);
    assert(RfidRecoveryPolicy::retryDelayMs(2) == 2000);
    assert(RfidRecoveryPolicy::retryDelayMs(3) == 4000);
    assert(RfidRecoveryPolicy::retryDelayMs(4) == 8000);
    assert(RfidRecoveryPolicy::retryDelayMs(5) == 10000);
    assert(RfidRecoveryPolicy::retryDelayMs(UINT8_MAX) == 10000);
}

static void test_failure_waits_and_success_stops_retries() {
    RfidRecoveryPolicy policy;
    policy.request(500, false);
    assert(policy.attemptDue(500));

    policy.recordFailure(500);
    assert(!policy.attemptDue(1499));
    assert(policy.attemptDue(1500));
    assert(policy.failureCount() == 1);

    policy.recordSuccess();
    assert(policy.ready());
    assert(!policy.attemptDue(UINT32_MAX));
}

static void test_force_request_preserves_ack_until_taken() {
    RfidRecoveryPolicy policy;
    policy.recordSuccess();
    policy.request(42, true);
    assert(!policy.ready());
    assert(policy.attemptDue(42));
    assert(policy.recoveryAckPending());

    policy.recordFailure(42);
    assert(policy.recoveryAckPending());
    policy.recordSuccess();
    assert(policy.takeRecoveryAck());
    assert(!policy.takeRecoveryAck());
}

static void test_deadline_check_handles_millis_rollover() {
    const uint32_t beforeWrap = UINT32_MAX - 50;
    const uint32_t afterWrapDeadline = beforeWrap + 100;
    assert(!RfidRecoveryPolicy::deadlineReached(beforeWrap + 40,
                                                afterWrapDeadline));
    assert(RfidRecoveryPolicy::deadlineReached(beforeWrap + 100,
                                               afterWrapDeadline));
    assert(RfidRecoveryPolicy::deadlineReached(beforeWrap + 101,
                                               afterWrapDeadline));
}

static void test_immediate_deadline_uses_current_long_uptime() {
    // A zero sentinel is no longer "due" after the signed half-range boundary.
    // Scheduling an immediate operation with the current millis() stays correct.
    const uint32_t longUptime = 0x80000010u;
    assert(!RfidRecoveryPolicy::deadlineReached(longUptime, 0));
    assert(RfidRecoveryPolicy::deadlineReached(longUptime, longUptime));

    RfidRecoveryPolicy policy;
    policy.request(longUptime, false);
    assert(policy.attemptDue(longUptime));
}

int main() {
    test_exponential_backoff_is_bounded();
    test_failure_waits_and_success_stops_retries();
    test_force_request_preserves_ack_until_taken();
    test_deadline_check_handles_millis_rollover();
    test_immediate_deadline_uses_current_long_uptime();
    return 0;
}
