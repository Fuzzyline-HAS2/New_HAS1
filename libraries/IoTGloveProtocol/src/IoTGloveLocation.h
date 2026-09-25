#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace iotglove {

struct BeaconMapEntry { char prefix; const char* room; };

// HAS3 device IDs use an uppercase room initial followed by a device suffix.
// Match the reference protocol's 2..18 ASCII letters/digits/underscore/hyphen.
inline const BeaconMapEntry* mappedBeacon(const char* advertisement,
                                          const BeaconMapEntry* map, size_t count) {
  if (!advertisement || strncmp(advertisement, "HAS3:", 5) != 0) return nullptr;
  const char* device = advertisement + 5;
  const size_t length = strlen(device);
  if (length < 2 || length > 18) return nullptr;
  for (size_t i = 0; i < length; ++i) {
    const char ch = device[i];
    if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return nullptr;
  }
  for (size_t i = 0; i < count; ++i)
    if (device[0] == map[i].prefix) return &map[i];
  return nullptr;
}

inline const char* mappedRoom(const char* advertisement,
                             const BeaconMapEntry* map, size_t count) {
  const BeaconMapEntry* entry = mappedBeacon(advertisement, map, count);
  return entry ? entry->room : nullptr;
}

struct Location {
  const char* room = "unknown";
  int16_t rssi = -127;
  uint32_t ageMs = 0;
  bool valid = false;
};

// Bounded port of updated_IoTglove's window/EMA/room selection algorithm.
class LocationTracker {
 public:
  static constexpr size_t kCapacity = 24;
  static constexpr size_t kMaxSamples = 96;
  static constexpr uint32_t kSampleWindowMs = 1500;
  static constexpr uint32_t kEvalIntervalMs = 200;
  static constexpr uint32_t kTtlMs = 5000;
  static constexpr uint32_t kSwitchHoldMs = 1200;
  static constexpr int kHysteresisDb = 5;

  explicit LocationTracker(const BeaconMapEntry* map, size_t count)
      : map_(map), mapCount_(count) {}

  void clear(uint32_t now = 0) {
    for (auto& item : devices_) item = Device{};
    sampleCount_ = 0;
    stable_ = candidate_ = -1;
    startedAt_ = now;
    haveObservation_ = evaluated_ = false;
    stableRssi_ = -127;
  }

  bool observe(const char* advertisement, int rssi, uint32_t now) {
    const BeaconMapEntry* mapping = mappedBeacon(advertisement, map_, mapCount_);
    if (!mapping || rssi > 0 || rssi < -127) return false;
    const size_t room = static_cast<size_t>(mapping - map_);
    if (room >= kCapacity) return false;
    const char* name = advertisement + 5;
    int slot = -1;
    for (size_t i = 0; i < kCapacity; ++i)
      if (strcmp(devices_[i].name, name) == 0) { slot = i; break; }
    if (slot < 0) {
      for (size_t i = 0; i < kCapacity; ++i)
        if (!devices_[i].name[0]) { slot = i; break; }
      if (slot < 0) return false;
      devices_[slot] = Device{};
      strcpy(devices_[slot].name, name);
    }
    devices_[slot].room = room;
    devices_[slot].lastSeen = now;
    lastObservation_ = now;
    haveObservation_ = true;
    size_t sample = sampleCount_;
    if (sampleCount_ < kMaxSamples) ++sampleCount_;
    else {
      sample = 0;
      for (size_t i = 1; i < kMaxSamples; ++i)
        if (now - samples_[i].at > now - samples_[sample].at) sample = i;
    }
    samples_[sample] = Sample{now, static_cast<int16_t>(rssi), static_cast<uint8_t>(slot)};
    return true;
  }

  Location update(uint32_t now) {
    if (!evaluated_ || now - lastEval_ >= kEvalIntervalMs) {
      evaluated_ = true;
      lastEval_ = now;
      evaluate(now);
    }
    if (stable_ < 0) return Location{};
    Location result;
    result.room = map_[stable_].room;
    result.rssi = stableRssi_;
    // Reference semantics: stable room survives missing room scores while ANY
    // valid HAS3 device keeps arriving. UART age follows that global freshness.
    result.ageMs = now - lastObservation_;
    result.valid = true;
    return result;
  }

 private:
  struct Sample {
    uint32_t at = 0;
    int16_t rssi = -127;
    uint8_t device = 0;
  };
  struct Device {
    char name[19] = {};
    size_t room = 0;
    uint32_t lastSeen = 0;
    float ema = -127.0f;
    bool hasEma = false;
  };
  struct RoomScore { bool active = false; float score = -127.0f; };

  void evaluate(uint32_t now) {
    size_t kept = 0;
    for (size_t i = 0; i < sampleCount_; ++i)
      if (now - samples_[i].at <= kSampleWindowMs) samples_[kept++] = samples_[i];
    sampleCount_ = kept;
    bool active[kCapacity] = {};
    for (size_t i = 0; i < kCapacity; ++i) {
      Device& device = devices_[i];
      if (!device.name[0]) continue;
      if (now - device.lastSeen > kTtlMs) { device = Device{}; continue; }
      int values[kMaxSamples];
      size_t count = 0;
      for (size_t j = 0; j < sampleCount_; ++j)
        if (samples_[j].device == i) values[count++] = samples_[j].rssi;
      if (count < 2) continue;
      active[i] = true;
      for (size_t j = 1; j < count; ++j) {
        const int value = values[j];
        size_t k = j;
        while (k > 0 && values[k - 1] > value) { values[k] = values[k - 1]; --k; }
        values[k] = value;
      }
      const int median = count % 2 ? values[count / 2] :
          (values[count / 2 - 1] + values[count / 2]) / 2;
      device.ema = device.hasEma ? device.ema * 0.5f + median * 0.5f : median;
      device.hasEma = true;
      // The reference refreshes EMA retention on evaluation as well as receipt.
      device.lastSeen = now;
    }
    RoomScore scores[kCapacity];
    int best = -1;
    for (size_t room = 0; room < mapCount_ && room < kCapacity; ++room) {
      float top1 = -127.0f, top2 = -127.0f;
      size_t count = 0;
      for (size_t i = 0; i < kCapacity; ++i) {
        if (!active[i] || devices_[i].room != room) continue;
        ++count;
        const float value = devices_[i].ema;
        if (value > top1) { top2 = top1; top1 = value; }
        else if (value > top2) top2 = value;
      }
      if (!count) continue;
      scores[room].active = true;
      scores[room].score = count >= 2 ? (top1 + top2) / 2.0f : top1;
      if (best < 0 || scores[room].score > scores[best].score) best = room;
    }
    if (best >= 0 && stable_ == -2) {
      stable_ = best;  // Recovery from timeout has no initial hold.
      candidate_ = -1;
    } else if (best < 0) {
      candidate_ = -1;
      if (now - (haveObservation_ ? lastObservation_ : startedAt_) >= kTtlMs)
        stable_ = -2;
    } else if (stable_ == best) {
      candidate_ = -1;
    } else if (stable_ < 0 || !scores[stable_].active ||
               scores[best].score >= scores[stable_].score + kHysteresisDb) {
      if (candidate_ != best) { candidate_ = best; candidateSince_ = now; }
      else if (now - candidateSince_ >= kSwitchHoldMs) {
        stable_ = best;
        candidate_ = -1;
      }
    } else {
      candidate_ = -1;
    }
    if (stable_ >= 0 && scores[stable_].active)
      stableRssi_ = static_cast<int16_t>(scores[stable_].score);
  }

  const BeaconMapEntry* map_;
  size_t mapCount_;
  Device devices_[kCapacity] = {};
  Sample samples_[kMaxSamples] = {};
  size_t sampleCount_ = 0;
  int stable_ = -1;  // -1 initial acquisition, -2 timed-out unknown.
  int candidate_ = -1;
  int16_t stableRssi_ = -127;
  uint32_t candidateSince_ = 0;
  uint32_t startedAt_ = 0;
  uint32_t lastObservation_ = 0;
  uint32_t lastEval_ = 0;
  bool haveObservation_ = false;
  bool evaluated_ = false;
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
