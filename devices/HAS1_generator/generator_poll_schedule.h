#ifndef GENERATOR_POLL_SCHEDULE_H
#define GENERATOR_POLL_SCHEDULE_H
#include <stdint.h>

// Completion-based scheduling: elapsed intervals never become queued work.
// The caller owns the one-in-flight flag; timestamps support millis rollover.
class GeneratorPollSchedule {
 public:
  void begin(uint32_t now) { last_ = now; wait_ = 2000; failures_ = 0; }
  bool due(uint32_t now) const { return uint32_t(now - last_) >= wait_; }
  void completed(uint32_t now, bool success, uint32_t jitter = 0) {
    if (success) failures_ = 0;
    else if (failures_ < 3) ++failures_;
    wait_ = success ? 2000 : (2000u << (failures_ - 1));
    // Bounded positive jitter only for failures; normal heartbeats stay at 2s.
    if (!success) wait_ += jitter % 201;
    last_ = now;
  }
  void rejected(uint32_t now) { last_ = now; wait_ = 250; }
  uint32_t interval() const { return wait_; }
 private:
  uint32_t last_ = 0, wait_ = 2000;
  uint8_t failures_ = 0;
};
#endif
