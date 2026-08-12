#include "encoder_hal.h"
#include "library_and_pin.h"

#define ENCODER_MAX 95
#define ENCODER_MIN 0

ESP32Encoder encoder;

void EncoderInit() {
    Log("ENC", "init");
    ESP32Encoder::useInternalWeakPullResistors = puType::up;
    encoder.attachFullQuad(ENCODER_B_PIN, ENCODER_A_PIN);  // A/B 순서 스왑 → 방향 반전 보정
    encoder.clearCount();
    pinMode(ENCODER_BUTTON_PIN, INPUT);
}

void EncoderHalUpdate() {}

void EncoderEnable() {
    encoder.resumeCount();
}

void EncoderDisable() {
    encoder.pauseCount();
}

long readEncoderValue() {
    long count = encoder.getCount();
    if (count > ENCODER_MAX * 4) {
        count = ENCODER_MAX * 4;
        encoder.setCount(count);
    } else if (count < ENCODER_MIN * 4) {
        count = ENCODER_MIN * 4;
        encoder.setCount(count);
    }
    return count / 4;
}

bool isEncoderButtonPressed() {
    return digitalRead(ENCODER_BUTTON_PIN) == LOW;  // 외부 풀업: 평소 HIGH, 누르면 LOW
}

void EncoderReset() {
    encoder.clearCount();
}
