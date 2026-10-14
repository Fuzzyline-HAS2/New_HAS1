#pragma once

#include <stdint.h>

namespace iotglove {

// Request a resend of the current LED frame; never changes game or motor state.
class PixelRefresh {
 public:
  static constexpr uint32_t kMotorIntervalMs = 500U;
  static constexpr uint32_t kAfterMotorMs = 50U;

  bool update(bool motor, bool visualChanged, uint32_t now) {
    if (motor != motor_) {
      motor_ = motor;
      if (motor) {
        lastRefresh_ = now;
        afterMotorPending_ = false;
      } else {
        motorStopped_ = now;
        afterMotorPending_ = true;
      }
    }
    const bool periodic = motor && uint32_t(now - lastRefresh_) >= kMotorIntervalMs;
    const bool afterMotor = afterMotorPending_ &&
        uint32_t(now - motorStopped_) >= kAfterMotorMs;
    if (!visualChanged && !periodic && !afterMotor) return false;
    lastRefresh_ = now;
    if (afterMotor) afterMotorPending_ = false;
    return true;
  }

 private:
  uint32_t lastRefresh_ = 0, motorStopped_ = 0;
  bool motor_ = false, afterMotorPending_ = false;
};

}  // namespace iotglove
