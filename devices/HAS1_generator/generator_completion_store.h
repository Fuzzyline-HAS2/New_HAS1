#ifndef GENERATOR_COMPLETION_STORE_H
#define GENERATOR_COMPLETION_STORE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Generator-local pending intent; not a general settings or crash namespace.
// Only the network owner calls these functions. Never send before save succeeds.
namespace GeneratorCompletionStore {
enum class Target : uint8_t { BatteryMax = 1, Repaired = 2 };
enum class LoadResult { Empty, Ready, Fault };
struct Intent {
  char deviceKey[65];
  char mac[18];
  char epoch[37];
  uint32_t revision;
  Target target;
  int8_t wireCount;
  int8_t maximum;
};

namespace Detail {
// Fixed byte encoding avoids compiler padding, alignment and enum ABI changes.
static const size_t RecordSize = 137;
static const size_t CrcOffset = 133;
inline bool hex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
inline size_t length(const char *text, size_t capacity) {
  size_t n = 0;
  while (n < capacity && text[n]) ++n;
  return n;
}
inline bool valid(const Intent &intent) {
  const size_t n = length(intent.deviceKey, sizeof(intent.deviceKey));
  if (n == 0 || n == sizeof(intent.deviceKey)) return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = intent.deviceKey[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  }
  if (length(intent.mac, sizeof(intent.mac)) != 17 ||
      length(intent.epoch, sizeof(intent.epoch)) != 36) return false;
  for (size_t i = 0; i < 17; ++i) {
    if (i % 3 == 2 ? intent.mac[i] != ':' : !hex(intent.mac[i])) return false;
  }
  for (size_t i = 0; i < 36; ++i) {
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? intent.epoch[i] != '-' : !hex(intent.epoch[i])) return false;
  }
  return (intent.target == Target::BatteryMax || intent.target == Target::Repaired) &&
         intent.maximum >= 1 && intent.maximum <= 4 &&
         intent.wireCount >= intent.maximum && intent.wireCount <= 4;
}
inline uint32_t crc(const uint8_t *data, size_t length) {
  uint32_t value = UINT32_MAX;
  for (size_t i = 0; i < length; ++i) {
    value ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit)
      value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
  }
  return ~value;
}
inline void put32(uint8_t *out, uint32_t value) {
  for (uint8_t i = 0; i < 4; ++i) out[i] = uint8_t(value >> (8 * i));
}
inline uint32_t get32(const uint8_t *in) {
  return uint32_t(in[0]) | (uint32_t(in[1]) << 8) |
         (uint32_t(in[2]) << 16) | (uint32_t(in[3]) << 24);
}
inline bool encode(const Intent &intent, uint8_t *record) {
  if (!valid(intent)) return false;
  memset(record, 0, RecordSize);
  memcpy(record, "GCPI", 4);
  record[4] = 1; // format version; unknown versions fail closed
  record[6] = uint8_t(intent.target);
  record[7] = uint8_t(intent.wireCount);
  record[8] = uint8_t(intent.maximum);
  memcpy(record + 9, intent.deviceKey, length(intent.deviceKey, sizeof(intent.deviceKey)));
  memcpy(record + 74, intent.mac, 17);
  memcpy(record + 92, intent.epoch, 36);
  put32(record + 129, intent.revision);
  put32(record + CrcOffset, crc(record, CrcOffset));
  return true;
}
inline bool decode(const uint8_t *record, Intent &intent) {
  if (memcmp(record, "GCPI", 4) || record[4] != 1 || record[5] != 0 ||
      get32(record + CrcOffset) != crc(record, CrcOffset)) return false;
  Intent candidate = {};
  memcpy(candidate.deviceKey, record + 9, sizeof(candidate.deviceKey));
  memcpy(candidate.mac, record + 74, sizeof(candidate.mac));
  memcpy(candidate.epoch, record + 92, sizeof(candidate.epoch));
  candidate.revision = get32(record + 129);
  candidate.target = Target(record[6]);
  candidate.wireCount = int8_t(record[7]);
  candidate.maximum = int8_t(record[8]);
  if (!valid(candidate)) return false;
  intent = candidate;
  return true;
}
} // namespace Detail

LoadResult load(Intent &intent);
bool save(const Intent &intent);
// Caller may clear a corrupt record only after a verified safe game boundary.
// An unconfirmed erase must keep new completion creation blocked.
bool clear();
} // namespace GeneratorCompletionStore

#ifndef GENERATOR_COMPLETION_STORE_HOST
#include <nvs.h>

namespace GeneratorCompletionStore {
namespace Detail {
static const char Namespace[] = "gen_complete";
static const char Key[] = "pending";
inline LoadResult readRecord(uint8_t *record) {
  nvs_handle_t handle;
  esp_err_t error = nvs_open(Namespace, NVS_READONLY, &handle);
  if (error == ESP_ERR_NVS_NOT_FOUND) return LoadResult::Empty;
  if (error != ESP_OK) return LoadResult::Fault;
  size_t size = 0;
  error = nvs_get_blob(handle, Key, nullptr, &size);
  if (error == ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return LoadResult::Empty;
  }
  if (error != ESP_OK || size != RecordSize) {
    nvs_close(handle);
    return LoadResult::Fault;
  }
  error = nvs_get_blob(handle, Key, record, &size);
  nvs_close(handle);
  return error == ESP_OK && size == RecordSize ? LoadResult::Ready : LoadResult::Fault;
}
} // namespace Detail

inline LoadResult load(Intent &intent) {
  uint8_t record[Detail::RecordSize];
  const LoadResult result = Detail::readRecord(record);
  if (result != LoadResult::Ready) return result;
  return Detail::decode(record, intent) ? LoadResult::Ready : LoadResult::Fault;
}

inline bool save(const Intent &intent) {
  uint8_t desired[Detail::RecordSize], current[Detail::RecordSize];
  if (!Detail::encode(intent, desired)) return false;
  const LoadResult existing = Detail::readRecord(current);
  if (existing == LoadResult::Fault) return false;
  // Retries never rewrite flash or replace an unresolved intent with a new one.
  if (existing == LoadResult::Ready) return memcmp(current, desired, sizeof(desired)) == 0;
  nvs_handle_t handle;
  if (nvs_open(Detail::Namespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  esp_err_t error = nvs_set_blob(handle, Detail::Key, desired, sizeof(desired));
  if (error == ESP_OK) error = nvs_commit(handle);
  nvs_close(handle);
  if (error != ESP_OK) return false;
  // Read through a fresh handle after commit; never treat a failed verify as saved.
  return Detail::readRecord(current) == LoadResult::Ready &&
         memcmp(current, desired, sizeof(desired)) == 0;
}

inline bool clear() {
  nvs_handle_t handle;
  const esp_err_t opened = nvs_open(Detail::Namespace, NVS_READWRITE, &handle);
  if (opened != ESP_OK) return false;
  esp_err_t error = nvs_erase_key(handle, Detail::Key);
  if (error == ESP_OK) error = nvs_commit(handle);
  else if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
  nvs_close(handle);
  if (error != ESP_OK) return false;
  uint8_t record[Detail::RecordSize];
  return Detail::readRecord(record) == LoadResult::Empty;
}
} // namespace GeneratorCompletionStore
#endif
#endif
