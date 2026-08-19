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
// 근접 인식 Dead Zone 대응용 RxGain 전환 (rfid.ino 구현). GainMode는 여기서 정의해야 한다 —
// Arduino가 .ino 파일들을 병합할 때 자동 생성 함수 프로토타입을 스케치 맨 앞(이 헤더 include
// 다음, 각 .ino 탭의 실제 코드보다 앞)에 삽입하므로, rfid.ino 안에서만 정의하면 그 프로토타입
// 자리에서 "GainMode를 아직 모른다"는 컴파일 에러가 난다.
enum GainMode { GAIN_NEAR, GAIN_FAR };
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
