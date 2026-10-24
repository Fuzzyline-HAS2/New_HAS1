#include "sensor.h"

#include <Arduino.h>
#include "game_state.h"
#include "blackbox.h"
#include "button_trace.h"
#include "library_and_pin.h"

namespace iotglove {
namespace {
DebouncedInput chipInput, buttonInput;
ButtonTrace buttonTrace;
}

void sensorConfigurePins() {
  pinMode(IOTGLOVE_CHIP_PIN, INPUT_PULLUP);
  pinMode(IOTGLOVE_BUTTON_PIN, INPUT_PULLUP);
}

void sensorBegin(uint32_t now) {
  chipInput.begin(digitalRead(IOTGLOVE_CHIP_PIN) == LOW, now);
  buttonInput.begin(digitalRead(IOTGLOVE_BUTTON_PIN) == LOW, now);
}

void sensorPoll(GameModel& game, uint32_t now, uint32_t inFlight) {
  if (chipInput.update(digitalRead(IOTGLOVE_CHIP_PIN) == LOW, now)) game.chipChanged(chipInput.value(), now);
  if (buttonInput.update(digitalRead(IOTGLOVE_BUTTON_PIN) == LOW, now)) {
    ButtonDecision decision{ButtonDisposition::Release, 0};
    if (buttonInput.value()) {
      const uint32_t id = buttonTrace.press(now);
      decision = game.buttonPressed(now, id, buttonTrace.selected());
    }
    if (buttonTrace.selected())
      blackBoxButton(buttonTrace.id(), buttonInput.value(), decision, game, inFlight);
  }
  const uint32_t suppressed = buttonTrace.suppressed(now);
  if (suppressed) blackBoxButtonSuppressed(suppressed);
}

bool sensorChipPresent() { return chipInput.value(); }
bool sensorButtonDown() { return buttonInput.value(); }
int sensorChipRaw() { return digitalRead(IOTGLOVE_CHIP_PIN); }
int sensorButtonRaw() { return digitalRead(IOTGLOVE_BUTTON_PIN); }
}  // namespace iotglove
