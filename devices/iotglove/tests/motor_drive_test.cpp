#include "../motor_drive.h"

#include <assert.h>
#include <stdio.h>

using namespace iotglove;
using namespace iotglove::motor_drive;

int main() {
  Settings s;  // rampMs 25, start 160, full 255.
  const uint32_t ramp = s.rampMs;
  const uint8_t start = s.startDuty;
  const uint32_t span = 255U - start;
  assert(rampDuty(0, s) == start);
  assert(rampDuty(ramp / 2, s) == start + (span * (ramp / 2)) / ramp);
  assert(rampDuty(ramp - 1, s) < 255);
  assert(rampDuty(ramp, s) == 255);
  assert(rampDuty(1000, s) == 255);

  Settings noRamp = s;
  noRamp.rampMs = 0;
  assert(rampDuty(0, noRamp) == 255);
  Settings inverted = s;
  inverted.startDuty = 255;
  inverted.fullDuty = 200;  // Misconfigured: never exceed fullDuty.
  assert(rampDuty(0, inverted) == 200);
  Settings equal = s;
  equal.startDuty = equal.fullDuty;
  assert(rampDuty(0, equal) == 255);
  Settings huge = s;
  huge.rampMs = 0xFFFFFFFFU;  // No 32-bit overflow in span * elapsed.
  assert(rampDuty(0xFFFFFFFEU, huge) >= start && rampDuty(0xFFFFFFFEU, huge) <= 255);

  SoftStart drive;
  assert(drive.update(false, MotorSource::None, 0) == 0 && !drive.on());
  // Proximity ramps from the first ON render; a stalled loop lands on the right duty.
  assert(drive.update(true, MotorSource::Proximity, 1000) == start && drive.on());
  assert(drive.update(true, MotorSource::Proximity, 1000 + ramp / 2) == rampDuty(ramp / 2, s));
  assert(drive.update(true, MotorSource::Proximity, 1500) == 255);
  // OFF is immediate and re-arms the ramp.
  assert(drive.update(false, MotorSource::None, 1600) == 0 && !drive.on());
  assert(drive.update(true, MotorSource::Proximity, 1700) == start);
  // Duty never steps down inside one ON: an instant source reaching full keeps full.
  assert(drive.update(true, MotorSource::Event, 1705) == 255);
  assert(drive.update(true, MotorSource::Proximity, 1710) == 255);
  assert(drive.update(false, MotorSource::None, 1800) == 0);
  // Ramp end also latches full, so a later OFF->ON re-arms (not sticky across pulses).
  assert(drive.update(true, MotorSource::Proximity, 2000) == start);
  assert(drive.update(true, MotorSource::Proximity, 2000 + ramp) == 255);
  drive.update(false, MotorSource::None, 2100);
  assert(drive.update(true, MotorSource::Proximity, 2200) == start);

  // Instant sources go straight to full on their first render.
  SoftStart instant;
  assert(instant.update(true, MotorSource::State, 5) == 255);
  instant.update(false, MotorSource::None, 6);
  assert(instant.update(true, MotorSource::Event, 7) == 255);
  instant.update(false, MotorSource::None, 8);
  assert(instant.update(true, MotorSource::Command, 9) == 255);
  instant.update(false, MotorSource::None, 10);
  assert(instant.update(true, MotorSource::Forced, 11) == start);  // Continuous ON soft starts.
  assert(instant.update(true, MotorSource::Forced, 11 + ramp) == 255);

  // millis() wrap inside a ramp.
  SoftStart wrap;
  assert(wrap.update(true, MotorSource::Proximity, UINT32_MAX - 9U) == start);
  assert(wrap.update(true, MotorSource::Proximity, 5U) == rampDuty(15, s));
  assert(wrap.update(true, MotorSource::Proximity, 15U) == 255);

  // Per-source policy is configurable.
  Settings all = s;
  all.softEvent = true;
  SoftStart soft(all);
  assert(soft.update(true, MotorSource::Event, 0) == start);
  Settings none = s;
  none.softProximity = none.softForced = false;
  SoftStart hard(none);
  assert(hard.update(true, MotorSource::Proximity, 0) == 255);

  puts("motor_drive_test passed");
  return 0;
}
