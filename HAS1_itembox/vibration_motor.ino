#include "vibration_hal.h"
#include "library_and_pin.h"

#define VIB_MOTOR_FREQ       5000
#define VIB_MOTOR_RESOLUTION 8

// AIN1->3.3V, AIN2->GND, STBY->3.3V 고정 배선 → PWMA 듀티로 세기만 조절
void vibration_motor_Init() {
    ledcAttach(MOTOR_PWMA_PIN, VIB_MOTOR_FREQ, VIB_MOTOR_RESOLUTION);
    ledcWrite(MOTOR_PWMA_PIN, 0);
}

void vibrationOn(uint8_t strength) {
    ledcWrite(MOTOR_PWMA_PIN, strength);
}

void vibrationOff() {
    ledcWrite(MOTOR_PWMA_PIN, 0);
}

void vibrationSetByEncoder(int answer) {
    int diff       = abs(answer - (int)readEncoderValue());
    int aRange     = modeValue[RANGE][ANSWER_RANGE];
    int vRange     = modeValue[RANGE][VIBRATION_RANGE];
    int vibeStrength;
    if      (diff < aRange + vRange * 0) vibeStrength = 0;
    else if (diff < aRange + vRange * 1) vibeStrength = 1;
    else if (diff < aRange + vRange * 2) vibeStrength = 2;
    else if (diff < aRange + vRange * 3) vibeStrength = 3;
    else                                 vibeStrength = 4;
    vibrationOn(modeValue[VIBESTREGNTH][vibeStrength]);
}
