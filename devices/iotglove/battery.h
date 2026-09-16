#pragma once

#include <stdint.h>

namespace iotglove {

// Actual calibrated ADC millivolts are passed in by the driver. Sixteen samples
// are averaged over time; a rail/out-of-range sample invalidates the whole batch.
class BatterySampler {
 public:
  BatterySampler(float divider, float calibration, uint32_t minMv, uint32_t maxMv)
      : divider_(divider), calibration_(calibration), minMv_(minMv), maxMv_(maxMv) {}
  bool configured() const {
    return divider_ > 1.0f && divider_ <= 20.0f && calibration_ > 0.5f &&
        calibration_ < 1.5f && minMv_ > 0 && maxMv_ > minMv_;
  }
  bool add(uint32_t adcMv, float& volts) {
    if (!configured()) return false;
    if (adcMv < 50U || adcMv > 3100U) invalid_ = true;
    sum_ += adcMv;
    if (++count_ != 16) return false;
    const float mv = (float(sum_) / 16.0f) * divider_ * calibration_;
    const bool valid = !invalid_ && mv >= minMv_ && mv <= maxMv_;
    sum_ = count_ = 0;
    invalid_ = false;
    if (valid) volts = mv / 1000.0f;
    return valid;
  }
 private:
  float divider_, calibration_;
  uint32_t minMv_, maxMv_;
  uint32_t sum_ = 0;
  uint8_t count_ = 0;
  bool invalid_ = false;
};

}  // namespace iotglove
