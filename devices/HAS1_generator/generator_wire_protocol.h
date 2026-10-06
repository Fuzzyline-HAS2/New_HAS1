#ifndef GENERATOR_WIRE_PROTOCOL_H
#define GENERATOR_WIRE_PROTOCOL_H
#include <stdint.h>
struct GeneratorWireSnapshot {
  char device[32] = {};
  char epoch[37] = {};
  char gameState[32] = {};
  char deviceState[32] = {};
  uint32_t revision = 0;
  int count = 0;
  int maximum = 0;
};
enum class GeneratorWireResult { Ok, Conflict, Failed };
#endif
