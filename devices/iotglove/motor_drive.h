#pragma once

#include <stdint.h>

#include "feedback.h"

namespace iotglove {
namespace motor_drive {

// Firmware-only tuning for the vibration motor PWM. The feedback engine still
// decides ON/OFF; this stage only shapes the duty of an ON request so the ERM
// motor starts from a duty that overcomes static friction and ramps to full.
// Soft start is applied per motor source: proximity and forced-ON requests
// are the frequent, long ones where lower inrush matters; state, event and
// operator-command pulses stay instant so 100-150 ms patterns keep their edge.
// The shortest soft-started pulse is the 100 ms proximity buzz, so the ramp
// must stay well under that; tune startDuty/rampMs on real gloves.
struct Settings {
  uint32_t rampMs = 25;      // Duration from startDuty to fullDuty; 0 disables the ramp.
  uint8_t startDuty = 160;   // First duty of a soft-started pulse (ERM dead zone is ~30-50%).
  uint8_t fullDuty = 255;    // Steady-state duty; 255 is full ON in 8-bit LEDC.
  bool softState = false;
  bool softEvent = false;
  bool softCommand = false;
  bool softProximity = true;
  bool softForced = true;
};

inline bool softStarts(MotorSource source, const Settings& settings) {
  switch (source) {
    case MotorSource::State: return settings.softState;
    case MotorSource::Event: return settings.softEvent;
    case MotorSource::Command: return settings.softCommand;
    case MotorSource::Proximity: return settings.softProximity;
    case MotorSource::Forced: return settings.softForced;
    case MotorSource::None: break;
  }
  return false;
}

// Duty for a request that has been ON for `elapsedMs`. Pure so a stalled loop
// (HTTP, flash) lands on the right duty instead of lagging per tick.
inline uint8_t rampDuty(uint32_t elapsedMs, const Settings& settings) {
  if (settings.rampMs == 0 || elapsedMs >= settings.rampMs || settings.startDuty >= settings.fullDuty)
    return settings.fullDuty;
  const uint64_t span = uint64_t(settings.fullDuty - settings.startDuty);
  return uint8_t(settings.startDuty + (span * elapsedMs) / settings.rampMs);
}

class SoftStart {
 public:
  explicit SoftStart(const Settings& settings = Settings{}) : settings_(settings) {}

  // Returns the LEDC duty (0..255) for this render. OFF is immediate: no ramp
  // down, so short gaps inside double pulses and the OTA/reset cut stay crisp.
  // Within one ON the duty never decreases: once an instant source (or the end
  // of the ramp) reached fullDuty, a soft source taking over keeps full.
  uint8_t update(bool requested, MotorSource source, uint32_t now) {
    if (!requested) {
      on_ = false;
      full_ = false;
      return 0;
    }
    if (!on_) {
      on_ = true;
      onSince_ = now;
    }
    if (full_) return settings_.fullDuty;
    const uint8_t duty = softStarts(source, settings_) ? rampDuty(uint32_t(now - onSince_), settings_)
                                                       : settings_.fullDuty;
    if (duty >= settings_.fullDuty) full_ = true;
    return duty;
  }

  bool on() const { return on_; }

 private:
  Settings settings_;
  bool on_ = false;
  bool full_ = false;
  uint32_t onSince_ = 0;
};

}  // namespace motor_drive
}  // namespace iotglove
