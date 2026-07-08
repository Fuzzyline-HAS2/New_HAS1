// PCNT 하드웨어 카운터(ESP32Encoder) 사용

void EncoderInit() {
    Log("ENC", "init");
    ESP32Encoder::useInternalWeakPullResistors = puType::up; // 풀업으로 ENCODER_A, ENCODER_B PIN 설정
    encoder.attachFullQuad(ENCODER_B_PIN, ENCODER_A_PIN);  // 한 틱 = 4카운트. A/B 순서를 바꿔 회전 방향 반전 (하드웨어가 리버스라서)
    encoder.clearCount(); // 엔코더의 현재 위치를 0으로 설정한다. 

    // GPIO34는 input-only 핀이라 내부 풀업 없음 → 회로에 외부 풀업 저항 달아둠
    pinMode(ENCODER_BUTTON_PIN, INPUT);
}

long readEncoderValue() {
    long count = encoder.getCount();  // 원시 카운트 (한 틱 = 4)
    if (count > ENCODER_MAX * 4) {
        count = ENCODER_MAX * 4;
        encoder.setCount(count); // 엔코더 값이 최댓값에 도달하면 최댓값으로 고정
    } else if (count < ENCODER_MIN * 4) {
        count = ENCODER_MIN * 4;
        encoder.setCount(count); // 엔코더 값이 최솟값에 도달하면 최솟값으로 고정
    }
    return count / 4;  // 이전 버전과 동일: 4카운트 = 1틱으로 환산
}

bool isEncoderButtonPressed() {
    return digitalRead(ENCODER_BUTTON_PIN) == LOW;  // 외부 풀업: 평소 HIGH, 누르면 LOW
}
