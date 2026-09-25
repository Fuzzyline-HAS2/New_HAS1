#pragma once

#include <stdint.h>
#include <string.h>

namespace tagmachine {
namespace ttgo_ota_record {

constexpr uint32_t kMagic = 0x54475431U;  // "TGT1"
constexpr size_t kCommandCapacity = 40;

struct Record {
  uint32_t magic = kMagic;
  uint32_t targetVersion = 0;
  uint32_t oldVersion = 0;
  uint32_t partitionVersion = 0;
  char command[kCommandCapacity] = {};
  uint32_t checksum = 0;
};

static_assert(sizeof(Record) == 60, "TTGO OTA NVS record layout changed");

inline uint32_t checksum(const Record &record) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&record);
  uint32_t value = 2166136261U;
  for (size_t i = 0; i < sizeof(record) - sizeof(record.checksum); ++i)
    value = (value ^ bytes[i]) * 16777619U;
  return value;
}

inline bool validVersion(uint32_t version) {
  return version > 0 && version <= 2147483647U;
}

inline bool valid(const Record &record) {
  return record.magic == kMagic && validVersion(record.targetVersion) &&
      validVersion(record.oldVersion) &&
      record.targetVersion > record.oldVersion &&
      validVersion(record.partitionVersion) && record.command[0] &&
      memchr(record.command, '\0', sizeof(record.command)) &&
      record.checksum == checksum(record);
}

inline Record make(uint32_t targetVersion, uint32_t oldVersion,
                   uint32_t partitionVersion, const char *command) {
  Record record;
  record.targetVersion = targetVersion;
  record.oldVersion = oldVersion;
  record.partitionVersion = partitionVersion;
  if (command && strlen(command) < sizeof(record.command))
    strcpy(record.command, command);
  record.checksum = checksum(record);
  return record;
}

inline bool bootProvesInstalled(const Record &record, uint32_t currentVersion,
                                uint32_t currentPartition) {
  return valid(record) && currentVersion == record.targetVersion &&
      currentVersion > record.oldVersion &&
      currentPartition == record.partitionVersion;
}

}  // namespace ttgo_ota_record
}  // namespace tagmachine
