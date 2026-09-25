#pragma once
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
inline std::vector<uint8_t> savedPreferences;
inline bool preferencesAvailable = true;
inline unsigned preferencesWrites = 0;
class Preferences {
 public:
  bool begin(const char* name, bool) { assert(std::string(name) == "revival-card"); return preferencesAvailable; }
  void end() {}
  size_t getBytesLength(const char* name) { assert(std::string(name) == "config"); return savedPreferences.size(); }
  size_t getBytes(const char*, void* data, size_t size) {
    if (size < savedPreferences.size()) return 0;
    memcpy(data, savedPreferences.data(), savedPreferences.size()); return savedPreferences.size();
  }
  size_t putBytes(const char*, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    savedPreferences.assign(bytes, bytes + size); ++preferencesWrites; return size;
  }
};
