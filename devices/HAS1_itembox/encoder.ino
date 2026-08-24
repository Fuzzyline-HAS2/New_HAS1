#include "hal.h"
#include "library_and_pin.h"

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

// 하드웨어 누적 카운트를 링(0~ENCODER_MAX) 위로 랩어라운드해서 반환.
// 더 이상 setCount()로 하드웨어 카운터를 강제로 되돌리지 않는다 — 최댓값을 넘겨 돌리면
// 0으로, 0에서 더 내리면 최댓값으로 자연스럽게 이어져야 하기 때문 (선형 클램프 대신 모듈로).
long readEncoderValue() {
    long count = encoder.getCount();
    const long ticks = (long)ENCODER_RANGE * 4;       // ESP32Encoder는 4카운트=1칸(quadrature)
    long wrapped = ((count % ticks) + ticks) % ticks;  // 음수 나머지 보정 포함 모듈로
    return wrapped / 4 + ENCODER_MIN;
}

bool isEncoderButtonPressed() {
    return digitalRead(ENCODER_BUTTON_PIN) == LOW;  // 외부 풀업: 평소 HIGH, 누르면 LOW
}

void EncoderReset() {
    encoder.clearCount();
}
