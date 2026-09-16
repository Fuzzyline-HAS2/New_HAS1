#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace iotglove {

struct BeaconMapEntry { const char* device; const char* room; };

inline const char* mappedRoom(const char* advertisement,
                             const BeaconMapEntry* map, size_t count) {
  if (!advertisement || strncmp(advertisement, "HAS3:", 5) != 0) return nullptr;
  const char* device = advertisement + 5;
  const size_t length = strlen(device);
  if (length == 0 || length > 18) return nullptr;
  for (size_t i = 0; i < count; ++i)
    if (strcmp(device, map[i].device) == 0) return map[i].room;
  return nullptr;
}

struct Location {
  const char* room = "unknown";
  int16_t rssi = -127;
  uint32_t ageMs = 0;
  bool valid = false;
};

// Fixed memory; entries hold indices into the immutable configuration map.
class LocationTracker {
 public:
  static constexpr size_t kCapacity = 24;
  static constexpr uint32_t kTtlMs = 5000;
  static constexpr uint32_t kSwitchHoldMs = 750;
  static constexpr int kHysteresisDb = 6;
  static constexpr int kMinRssi = -92;

  explicit LocationTracker(const BeaconMapEntry* map, size_t count)
      : map_(map), mapCount_(count) {}

  void clear() {
    for (auto& item : entries_) item = Entry{};
    current_ = -1;
    challenger_ = -1;
  }

  bool observe(const char* advertisement, int rssi, uint32_t now) {
    if (!mappedRoom(advertisement, map_, mapCount_) || rssi > 0 || rssi < -127)
      return false;
    size_t mapIndex = 0;
    while (mapIndex < mapCount_ && strcmp(advertisement + 5, map_[mapIndex].device))
      ++mapIndex;
    int slot = -1;
    for (size_t i = 0; i < kCapacity; ++i)
      if (entries_[i].used && entries_[i].mapIndex == mapIndex) slot = i;
    if (slot < 0) {
      for (size_t i = 0; i < kCapacity; ++i) {
        if (!entries_[i].used || now - entries_[i].lastSeen >= kTtlMs) {
          slot = i;
          break;
        }
      }
      if (slot < 0) return false;
      if (slot == current_) current_ = -1;
      if (slot == challenger_) challenger_ = -1;
      entries_[slot] = Entry{};
      entries_[slot].mapIndex = mapIndex;
      entries_[slot].used = true;
    }
    Entry& entry = entries_[slot];
    const bool stale = now - entry.lastSeen >= kTtlMs;
    if (stale) entry.samples = 0;
    entry.lastSeen = now;
    entry.history[entry.cursor] = rssi;
    entry.cursor = (entry.cursor + 1) % 3;
    if (entry.samples < 3) ++entry.samples;
    int filtered = rssi;
    if (entry.samples == 3) {
      int a = entry.history[0], b = entry.history[1], c = entry.history[2];
      if (a > b) { const int t = a; a = b; b = t; }
      if (b > c) { const int t = b; b = c; c = t; }
      if (a > b) { const int t = a; a = b; b = t; }
      filtered = b;
    }
    entry.average = entry.samples == 1 ? filtered : (entry.average * 3 + filtered) / 4;
    return true;
  }

  Location update(uint32_t now) {
    int best = -1;
    for (size_t i = 0; i < kCapacity; ++i)
      if (fresh(i, now) && (best < 0 || entries_[i].average > entries_[best].average))
        best = i;
    if (!fresh(current_, now)) {
      current_ = best;
      challenger_ = -1;
    } else if (best >= 0 && best != current_ &&
               entries_[best].average >= entries_[current_].average + kHysteresisDb) {
      if (challenger_ != best) { challenger_ = best; challengeSince_ = now; }
      if (now - challengeSince_ >= kSwitchHoldMs && entries_[best].samples >= 2) {
        current_ = best;
        challenger_ = -1;
      }
    } else {
      challenger_ = -1;
    }
    if (current_ < 0) return Location{};
    const Entry& entry = entries_[current_];
    Location result;
    result.room = map_[entry.mapIndex].room;
    result.rssi = entry.average;
    result.ageMs = now - entry.lastSeen;
    result.valid = true;
    return result;
  }

 private:
  struct Entry {
    size_t mapIndex = 0;
    uint32_t lastSeen = 0;
    int16_t history[3] = {};
    int16_t average = -127;
    uint8_t cursor = 0;
    uint8_t samples = 0;
    bool used = false;
  };
  bool fresh(int index, uint32_t now) const {
    return index >= 0 && entries_[index].used &&
           now - entries_[index].lastSeen < kTtlMs && entries_[index].average >= kMinRssi;
  }

  const BeaconMapEntry* map_;
  size_t mapCount_;
  Entry entries_[kCapacity] = {};
  int current_ = -1;
  int challenger_ = -1;
  uint32_t challengeSince_ = 0;
};

// Booting while HIGH must never create a reset loop. Arm only after stable LOW.
class ResetRequestGate {
 public:
  bool update(bool high, uint32_t now) {
    if (!initialized_ || high != previousHigh_) {
      initialized_ = true;
      previousHigh_ = high;
      levelSince_ = now;
    }
    if (latched_) return true;
    if (!high && now - levelSince_ >= 50) armed_ = true;
    if (high && armed_ && now - levelSince_ >= 20) latched_ = true;
    return latched_;
  }
 private:
  bool initialized_ = false;
  bool previousHigh_ = false;
  bool armed_ = false;
  bool latched_ = false;
  uint32_t levelSince_ = 0;
};
}  // namespace iotglove
