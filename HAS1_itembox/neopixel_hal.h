#pragma once

enum NeoColor { BLACK = 0, WHITE, RED, YELLOW, GREEN, BLUE, PURPLE };
enum NeoStrip  { NEO_PN532 = 0, NEO_ENCODER, NEO_INNER };

void NeopixelInit();
void NeoSetAll(NeoColor c);
void NeoSet(NeoStrip strip, NeoColor c);
void NeoSetBrightness(int brightness);
void NeoEncoderUpdate();
void BlinkHalUpdate();
