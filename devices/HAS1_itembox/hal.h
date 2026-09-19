#pragma once

// ── Neopixel ─────────────────────────────────────────────────────────────
// DEFAULT_BRIGHTNESS: 부팅 직후 및 서버 brightness 값이 유효 범위(1~100)를 벗어날 때
// 적용하는 기준 밝기 (raw 0~255). 다른 HAS1 device와 동일한 값으로 맞춘다.
#define DEFAULT_BRIGHTNESS 50

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
bool RfidPeekTag(uint8_t data[32]);
bool RfidScanNeeded();  // 연출/모터 이동 전용 gameState에서는 false — loop()가 스캔 자체를 건너뜀

// ── Encoder ──────────────────────────────────────────────────────────────
#define ENCODER_MAX 95
#define ENCODER_MIN 0
#define ENCODER_RANGE (ENCODER_MAX - ENCODER_MIN + 1)  // 링 전체 칸 수 (96)

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
