#ifndef GENERATOR_WIRE_STATE_H
#define GENERATOR_WIRE_STATE_H
#include <stdint.h>

// Physical observation and acknowledged server value have separate lifetimes.
class GeneratorWireState {
 public:
  static const uint32_t SampleMs = 100;
  static const uint32_t StableMs = 1000;
  static const uint32_t MaxSampleGapMs = 250;
  GeneratorWireState() { reset(); }
  void reset() {
    sampled_ = qualified_ = acknowledged_ = false;
    raw_ = stable_ = acknowledgedCount_ = -1;
    lastSample_ = candidateSince_ = 0;
    ++generation_;
  }
  bool due(uint32_t now) const { return !sampled_ || uint32_t(now-lastSample_) >= SampleMs; }
  bool sample(uint32_t now, int raw) {
    if (raw < 0 || raw > 4) { reset(); return false; }
    const bool gap = sampled_ && uint32_t(now-lastSample_) > MaxSampleGapMs;
    if (!sampled_ || gap || raw != raw_) {
      candidateSince_ = now;
      qualified_ = false;
    }
    sampled_ = true; lastSample_ = now; raw_ = raw;
    if (uint32_t(now-candidateSince_) < StableMs) return false;
    qualified_ = true;
    if (stable_ == raw) return false;
    stable_ = raw;
    return true;
  }
  bool qualified(uint32_t now) const {
    return sampled_ && qualified_ && raw_ == stable_ && uint32_t(now-lastSample_) <= MaxSampleGapMs;
  }
  bool acknowledge(int count, uint32_t generation) {
    if (generation != generation_ || count < 0 || count > 4) return false;
    acknowledged_ = true; acknowledgedCount_ = count; return true;
  }
  void invalidateAck() { acknowledged_ = false; }
  bool acknowledged() const { return acknowledged_; }
  bool synced() const { return acknowledged_ && acknowledgedCount_ == stable_; }
  bool ready(uint32_t now, int minimum) const {
    return minimum >= 1 && minimum <= 4 && qualified(now) && synced() && stable_ >= minimum;
  }
  int stable() const { return stable_; }
  int raw() const { return raw_; }
  int acknowledgedCount() const { return acknowledgedCount_; }
  uint32_t generation() const { return generation_; }
 private:
  bool sampled_, qualified_, acknowledged_;
  int raw_, stable_, acknowledgedCount_;
  uint32_t lastSample_, candidateSince_;
  uint32_t generation_ = 0;
};
#endif
