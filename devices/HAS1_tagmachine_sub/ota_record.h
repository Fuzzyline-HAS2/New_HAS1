#pragma once

#include <stdint.h>

namespace tagmachine {
namespace ota_record {

enum class Result : uint8_t {
    None,
    Accepted,
    Flashing,
    Updated,
    Skipped,
    Failed,
    Disabled,
};

// The magic belongs only to the TagMachine Beetle NVS schema. Change it and
// add an explicit migration before changing the layout below.
constexpr uint32_t kMagic = 0x54474f31U;  // "TGO1"

struct Record {
    uint32_t magic = kMagic;
    uint32_t request = 0;
    uint32_t requestedVersion = 0;  // Zero selects the fixed latest release.
    uint32_t targetVersion = 0;     // Filled after latest-release skip/update.
    uint32_t oldVersion = 0;
    uint32_t oldBootId = 0;
    uint32_t partitionVersion = 0;
    Result result = Result::None;
    uint8_t reserved[3] = {};
};

static_assert(sizeof(Record) == 32, "TagMachine OTA NVS record layout changed");

inline bool pending(Result result) {
    return result == Result::Accepted || result == Result::Flashing;
}

inline bool validVersion(uint32_t version) {
    return version > 0 && version <= 2147483647U;
}

inline bool valid(const Record &record) {
    return record.magic == kMagic && record.request != 0 &&
           (record.requestedVersion == 0 || validVersion(record.requestedVersion)) &&
           (record.targetVersion == 0 || validVersion(record.targetVersion)) &&
           (record.requestedVersion == 0 ||
            record.requestedVersion == record.targetVersion) &&
           static_cast<unsigned>(record.result) <=
               static_cast<unsigned>(Result::Disabled);
}

// A pending attempt becomes Updated only after a newer firmware version starts
// in a different boot, using the same USB-installed partition schema.
// Every ambiguous/power-loss case is a safe failure, never an implicit success.
inline Result recover(const Record &record, uint32_t currentVersion,
                      uint32_t currentPartition, uint32_t currentBootId) {
    if (!valid(record)) return Result::None;
    if (pending(record.result)) {
        const bool targetMatches = validVersion(record.targetVersion) &&
                                   currentVersion == record.targetVersion;
        const bool installed = targetMatches && validVersion(currentVersion) &&
                               validVersion(record.oldVersion) &&
                               currentVersion > record.oldVersion &&
                               currentBootId != record.oldBootId &&
                               currentPartition == record.partitionVersion;
        return installed ? Result::Updated : Result::Failed;
    }
    if (record.result == Result::Updated || record.result == Result::Skipped) {
        if (!validVersion(record.targetVersion) ||
            currentVersion != record.targetVersion ||
            currentPartition != record.partitionVersion) {
            return Result::Failed;
        }
    }
    return record.result;
}

inline bool sameRequest(const Record &record, uint32_t request,
                        uint32_t requestedVersion = 0) {
    return record.request == request &&
           record.requestedVersion == requestedVersion;
}

}  // namespace ota_record
}  // namespace tagmachine
