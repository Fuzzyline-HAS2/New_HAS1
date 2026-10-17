#ifndef GENERATOR_WIRE_STATE_H
#define GENERATOR_WIRE_STATE_H
#include <stdint.h>

// Per-connector observed votes and the acknowledged server count have separate lifetimes.
class GeneratorWireState {
 public:
  static const uint32_t SampleMs = 100;
  static const uint32_t StableMs = 1000;
  static const uint8_t WindowSamples = 10;
  static const uint8_t RequiredVotes = 8;
  GeneratorWireState() { reset(); }
  void reset() {
    sampled_ = qualified_ = acknowledged_ = startupComplete_ = false;
    stable_ = acknowledgedCount_ = -1;
    lastSample_ = firstSample_ = 0;
    sampleCount_ = nextSample_ = lastMask_ = 0;
    for (uint8_t i = 0; i < WindowSamples; ++i) masks_[i] = 0;
    ++generation_;
  }
  bool due(uint32_t now) const { return !sampled_ || uint32_t(now-lastSample_) >= SampleMs; }
  bool sampleMask(uint32_t now, uint8_t mask) {
    if (mask > 0x0f) { reset(); return false; }
    if (!sampled_) firstSample_ = now;
    sampled_ = true; lastSample_ = now; lastMask_ = mask;
    masks_[nextSample_] = mask;
    nextSample_ = (nextSample_ + 1) % WindowSamples;
    if (sampleCount_ < WindowSamples) ++sampleCount_;
    qualified_ = false;
    if (sampleCount_ < WindowSamples) return false;
    if (!startupComplete_) {
      if (uint32_t(now-firstSample_) < StableMs) return false;
      startupComplete_ = true;
    }
    int connected = 0;
    for (uint8_t pin = 0; pin < 4; ++pin) {
      uint8_t votes = 0;
      for (uint8_t i = 0; i < WindowSamples; ++i) {
        if (masks_[i] & (1u << pin)) ++votes;
      }
      if (votes >= RequiredVotes) ++connected;
      else if (WindowSamples - votes < RequiredVotes) return false;
    }
    qualified_ = true;
    if (stable_ == connected) return false;
    stable_ = connected;
    return true;
  }
  // No age expiry: only a new real window or reset changes the vote verdict.
  bool qualified(uint32_t /*now*/) const { return sampled_ && qualified_; }
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
  uint8_t rawMask() const { return lastMask_; }
  int acknowledgedCount() const { return acknowledgedCount_; }
  uint32_t generation() const { return generation_; }
  // Diagnostics operate on a copied snapshot, never from inside the sampler.
  uint8_t sampleCount() const { return sampleCount_; }
  uint32_t sampleAge(uint32_t now) const { return sampled_ ? uint32_t(now-lastSample_) : UINT32_MAX; }
  uint8_t connectedVotes(uint8_t pin) const {
    if (pin >= 4) return 0;
    uint8_t votes=0;
    for (uint8_t i=0;i<sampleCount_;++i) if (masks_[i] & (1u<<pin)) ++votes;
    return votes;
  }
 private:
  bool sampled_, qualified_, acknowledged_, startupComplete_;
  int stable_, acknowledgedCount_;
  uint8_t masks_[WindowSamples], sampleCount_, nextSample_, lastMask_;
  uint32_t lastSample_, firstSample_;
  uint32_t generation_ = 0;
};
#endif
