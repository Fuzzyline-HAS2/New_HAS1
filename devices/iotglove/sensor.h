#pragma once

#include <stdint.h>

namespace iotglove {
class GameModel;

// Keep pin configuration separate from the first sample so startup order is unchanged.
void sensorConfigurePins();
void sensorBegin(uint32_t now);
void sensorPoll(GameModel& game, uint32_t now, uint32_t inFlight = 0);  // Chip first, then button; one event per stable edge.
bool sensorChipPresent();
bool sensorButtonDown();
int sensorChipRaw();    // Immediate read for diagnostics; does not update debounce state.
int sensorButtonRaw();
}  // namespace iotglove
