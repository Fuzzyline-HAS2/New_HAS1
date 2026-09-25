#include <assert.h>
#include <stdint.h>

#include "../ota_record.h"

using tagmachine::ota_record::Record;
using tagmachine::ota_record::Result;

static Record pendingRecord(Result result = Result::Accepted) {
    Record record;
    record.request = 100;
    record.oldVersion = 3;
    record.oldBootId = 10;
    record.partitionVersion = 1;
    record.targetVersion = 4;
    record.result = result;
    return record;
}

static void test_pending_attempt_requires_version_boot_and_partition_proof() {
    Record record = pendingRecord();
    assert(tagmachine::ota_record::recover(record, 4, 1, 11) == Result::Updated);
    assert(tagmachine::ota_record::recover(record, 3, 1, 11) == Result::Failed);
    assert(tagmachine::ota_record::recover(record, 2, 1, 11) == Result::Failed);
    assert(tagmachine::ota_record::recover(record, 4, 1, 10) == Result::Failed);
    assert(tagmachine::ota_record::recover(record, 4, 2, 11) == Result::Failed);

    record.result = Result::Flashing;
    assert(tagmachine::ota_record::recover(record, 4, 1, 11) == Result::Updated);
    record.oldVersion = 0;
    assert(tagmachine::ota_record::recover(record, 4, 1, 11) == Result::Failed);
}

static void test_reset_immediately_after_durable_accepted_is_reconciled() {
    // OtaRequest persists this record before transmitting Accepted. Simulate a
    // reset immediately after that acknowledgement, before the worker starts.
    Record accepted = pendingRecord(Result::Accepted);
    accepted.targetVersion = 0;  // Target manifest was not authenticated yet.
    assert(tagmachine::ota_record::recover(accepted, 3, 1, 11) == Result::Failed);
    assert(tagmachine::ota_record::recover(accepted, 4, 1, 11) == Result::Failed);
    accepted.targetVersion = 4;
    assert(tagmachine::ota_record::recover(accepted, 4, 1, 11) == Result::Updated);
}

static void test_terminal_result_is_bound_to_running_image() {
    Record record = pendingRecord(Result::Skipped);
    record.targetVersion = 3;
    assert(tagmachine::ota_record::recover(record, 3, 1, 10) == Result::Skipped);
    assert(tagmachine::ota_record::recover(record, 3, 1, 11) == Result::Skipped);
    assert(tagmachine::ota_record::recover(record, 4, 1, 11) == Result::Failed);
    assert(tagmachine::ota_record::recover(record, 3, 2, 11) == Result::Failed);

    record.result = Result::Updated;
    record.targetVersion = 4;
    assert(tagmachine::ota_record::recover(record, 4, 1, 12) == Result::Updated);
    assert(tagmachine::ota_record::recover(record, 5, 1, 13) == Result::Failed);
}

static void test_invalid_and_conflicting_records_never_succeed() {
    Record record;
    assert(!tagmachine::ota_record::valid(record));
    assert(tagmachine::ota_record::recover(record, 4, 1, 11) == Result::None);

    record = pendingRecord();
    record.magic = 0;
    assert(!tagmachine::ota_record::valid(record));
    record = pendingRecord();
    record.requestedVersion = 4;
    record.targetVersion = 5;
    assert(!tagmachine::ota_record::valid(record));
    record = pendingRecord();
    record.requestedVersion = 2147483648U;
    assert(!tagmachine::ota_record::valid(record));
}

static void test_request_identity_is_exact_and_idempotent() {
    Record record = pendingRecord(Result::Failed);
    assert(tagmachine::ota_record::sameRequest(record, 100));
    assert(!tagmachine::ota_record::sameRequest(record, 101));
    assert(!tagmachine::ota_record::sameRequest(record, 100, 4));

    record.requestedVersion = 4;
    record.targetVersion = 4;
    assert(tagmachine::ota_record::sameRequest(record, 100, 4));
}

int main() {
    test_pending_attempt_requires_version_boot_and_partition_proof();
    test_reset_immediately_after_durable_accepted_is_reconciled();
    test_terminal_result_is_bound_to_running_image();
    test_invalid_and_conflicting_records_never_succeed();
    test_request_identity_is_exact_and_idempotent();
    return 0;
}
