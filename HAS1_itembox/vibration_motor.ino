// AIN1->3.3V, AIN2->GND, STBY->3.3V 고정 배선이라 방향은 정방향 고정, PWMA 듀티로 세기만 조절

void vibration_motor_Init() {
  ledcAttach(MOTOR_PWMA_PIN, VIB_MOTOR_FREQ, VIB_MOTOR_RESOLUTION);
  ledcWrite(MOTOR_PWMA_PIN, 0);
}

void vibrationOn(uint8_t strength) {  // strength: 0~255
  ledcWrite(MOTOR_PWMA_PIN, strength);
}

void vibrationOff() {
  ledcWrite(MOTOR_PWMA_PIN, 0);
}

// 엔코더 위치가 정답에 가까울수록 진동을 강하게 출력
void EncoderVibrationStrength(int answer)
{
    int differenceValue = abs(answer - (int)readEncoderValue());
    int answerRange = modeValue[RANGE][ANSWER_RANGE];
    int vibeRange = modeValue[RANGE][VIBRATION_RANGE];
    int vibeStrength = 0;
    if(differenceValue < answerRange + vibeRange * 0)       vibeStrength = 0;
    else if(differenceValue < answerRange + vibeRange * 1)  vibeStrength = 1;
    else if(differenceValue < answerRange + vibeRange * 2)  vibeStrength = 2;
    else if(differenceValue < answerRange + vibeRange * 3)  vibeStrength = 3;
    else vibeStrength = 4;
    vibrationOn(modeValue[VIBESTREGNTH][vibeStrength]);
}
