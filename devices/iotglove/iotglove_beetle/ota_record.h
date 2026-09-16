#pragma once

#include <stdint.h>
#include <string.h>

namespace beetle {
namespace ota_record {

enum class Result : uint8_t { None, Accepted, Flashing, Updated, Skipped, Failed, Disabled };
constexpr uint32_t kMagic = 0x49474F32;
constexpr uint32_t kLegacyMagic = 0x49474F31;

// One NVS blob preserves request identity and exact destination across power loss.
// Do not reorder fields without changing kMagic and adding a migration below.
struct Record {
  uint32_t magic = kMagic;
  uint32_t request = 0;
  uint32_t requestedVersion = 0;  // Zero is the legacy latest-release command.
  uint32_t targetVersion = 0;     // Pinned target, or observed result of latest.
  uint32_t oldVersion = 0;
  uint32_t oldBootId = 0;
  uint32_t partitionVersion = 0;
  Result result = Result::None;
  uint8_t reserved[3] = {};
};
static_assert(sizeof(Record) == 32, "NVS OTA record layout changed");

struct LegacyRecord {
  uint32_t magic;
  uint32_t request;
  uint32_t oldVersion;
  Result result;
  uint8_t reserved[3];
};
static_assert(sizeof(LegacyRecord) == 16, "Legacy NVS record layout changed");

inline bool pending(Result result) {
  return result == Result::Accepted || result == Result::Flashing;
}

inline bool validVersion(uint32_t version) {
  return version > 0 && version <= 2147483647U;
}

inline bool valid(const Record& record) {
  return record.magic == kMagic && record.request != 0 &&
      (record.requestedVersion == 0 || validVersion(record.requestedVersion)) &&
      (record.targetVersion == 0 || validVersion(record.targetVersion)) &&
      (record.requestedVersion == 0 || record.requestedVersion == record.targetVersion) &&
      static_cast<unsigned>(record.result) <= static_cast<unsigned>(Result::Disabled);
}

inline Record migrateLegacy(const LegacyRecord& legacy) {
  Record record;
  if (legacy.magic == kLegacyMagic && legacy.request) {
    record.request = legacy.request;
    record.oldVersion = legacy.oldVersion;
    // A v1 record has no validated target or source boot ID. Preserve identity,
    // but never infer successful rollback/installation from a version change.
    record.result = Result::Failed;
  }
  return record;
}

inline Result recover(const Record& record, uint32_t currentVersion,
                      uint32_t currentPartition, uint32_t currentBootId) {
  if (!valid(record)) return Result::None;
  if (pending(record.result)) {
    const bool targetMatches = record.requestedVersion == 0 ||
        currentVersion == record.targetVersion;
    const bool installed = targetMatches && validVersion(currentVersion) &&
        validVersion(record.oldVersion) && currentVersion != record.oldVersion &&
        currentBootId != record.oldBootId && currentPartition == record.partitionVersion;
    return installed ? Result::Updated : Result::Failed;
  }
  if (record.result == Result::Updated || record.result == Result::Skipped) {
    if (!validVersion(record.targetVersion) || currentVersion != record.targetVersion ||
        currentPartition != record.partitionVersion) return Result::Failed;
  }
  return record.result;
}

inline bool sameRequest(const Record& record, uint32_t request, uint32_t target) {
  return record.request == request && record.requestedVersion == target;
}

}  // namespace ota_record
}  // namespace beetle
