#include "sensor.h"

#include <Arduino.h>
#include "game_state.h"
#include "library_and_pin.h"

namespace iotglove {
namespace {
DebouncedInput chipInput, buttonInput;
}

void sensorConfigurePins() {
  pinMode(IOTGLOVE_CHIP_PIN, INPUT_PULLUP);
  pinMode(IOTGLOVE_BUTTON_PIN, INPUT_PULLUP);
}

void sensorBegin(uint32_t now) {
  chipInput.begin(digitalRead(IOTGLOVE_CHIP_PIN) == LOW, now);
  buttonInput.begin(digitalRead(IOTGLOVE_BUTTON_PIN) == LOW, now);
}

void sensorPoll(GameModel& game, uint32_t now) {
  if (chipInput.update(digitalRead(IOTGLOVE_CHIP_PIN) == LOW, now)) game.chipChanged(chipInput.value(), now);
  if (buttonInput.update(digitalRead(IOTGLOVE_BUTTON_PIN) == LOW, now) && buttonInput.value()) game.buttonPressed(now);
}

bool sensorChipPresent() { return chipInput.value(); }
bool sensorButtonDown() { return buttonInput.value(); }
int sensorChipRaw() { return digitalRead(IOTGLOVE_CHIP_PIN); }
int sensorButtonRaw() { return digitalRead(IOTGLOVE_BUTTON_PIN); }
}  // namespace iotglove
