#include "iotglove_beetle/ota_record.h"
#include <assert.h>

using namespace beetle::ota_record;

int main() {
  Record record;
  record.request = 100;
  record.requestedVersion = 4;
  record.targetVersion = 4;
  record.oldVersion = 9;
  record.oldBootId = 21;
  record.partitionVersion = 1;
  record.result = Result::Accepted;
  assert(recover(record, 4, 1, 22) == Result::Updated);  // Explicit rollback.
  assert(recover(record, 5, 1, 22) == Result::Failed);   // Changed != requested.
  assert(recover(record, 4, 1, 21) == Result::Failed);   // No new boot.
  assert(recover(record, 4, 2, 22) == Result::Failed);   // Partition mismatch.
  assert(recover(record, 9, 1, 22) == Result::Failed);   // Interrupted pre-commit.
  record.result = Result::Flashing;
  assert(recover(record, 4, 1, 22) == Result::Updated);
  record.oldVersion = 4;
  assert(recover(record, 4, 1, 22) == Result::Failed);   // Same version isn't a flash.
  record.oldVersion = 9;
  record.targetVersion = 0;
  assert(recover(record, 4, 1, 22) == Result::None);     // Pinned target missing.
  record.targetVersion = 4;
  record.result = Result::Skipped;
  assert(recover(record, 4, 1, 22) == Result::Skipped);
  assert(recover(record, 9, 1, 22) == Result::Failed);
  assert(recover(record, 4, 2, 22) == Result::Failed);
  record.result = Result::Updated;
  assert(recover(record, 4, 1, 23) == Result::Updated);  // Retry after another reboot.
  assert(sameRequest(record, 100, 4));
  assert(!sameRequest(record, 100, 0));
  assert(!sameRequest(record, 101, 4));
  record.requestedVersion = 0;
  assert(sameRequest(record, 100, 0));
  record.targetVersion = 0;
  record.result = Result::Accepted;
  assert(recover(record, 10, 1, 22) == Result::Updated); // Legacy latest command.
  assert(recover(record, 9, 1, 22) == Result::Failed);
  assert(recover(record, 10, 1, 21) == Result::Failed);
  record.requestedVersion = 2147483648U;
  assert(!valid(record));
  assert(recover(record, 4, 1, 22) == Result::None);

  LegacyRecord legacy = {};
  legacy.magic = kLegacyMagic;
  legacy.request = 200;
  legacy.oldVersion = 9;
  legacy.result = Result::Flashing;
  const Record migrated = migrateLegacy(legacy);
  assert(migrated.request == 200 && migrated.result == Result::Failed);
  assert(recover(migrated, 4, 1, 22) == Result::Failed);
  legacy.result = Result::Updated;
  assert(migrateLegacy(legacy).result == Result::Failed);
  legacy.magic = 0;
  assert(migrateLegacy(legacy).request == 0);
}
