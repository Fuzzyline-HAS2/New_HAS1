#pragma once

// ── Neopixel ─────────────────────────────────────────────────────────────
enum NeoColor { BLACK = 0, WHITE, RED, YELLOW, GREEN, BLUE, PURPLE };
enum NeoStrip  { NEO_PN532 = 0, NEO_ENCODER, NEO_INNER };

void NeopixelInit();
void NeoSetAll(NeoColor c);
void NeoSet(NeoStrip strip, NeoColor c);
void NeoSetBrightness(int brightness);
void NeoEncoderUpdate(long value);
void BlinkHalUpdate();

// ── Motor (Liner) ────────────────────────────────────────────────────────
void MotorInit();
void MotorHalUpdate();
void boxOpen();
void boxClose();
bool isBoxOpened();

// ── RFID ─────────────────────────────────────────────────────────────────
void RfidInit();
void RfidHalUpdate();
bool RfidTagPresent();
bool RfidReadTag(uint8_t data[32]);

// ── Encoder ──────────────────────────────────────────────────────────────
void EncoderInit();
void EncoderHalUpdate();
void EncoderEnable();
void EncoderDisable();
long readEncoderValue();
bool isEncoderButtonPressed();
void EncoderReset();

// ── Vibration Motor ──────────────────────────────────────────────────────
void vibration_motor_Init();
void vibrationOn(uint8_t strength);
void vibrationOff();
