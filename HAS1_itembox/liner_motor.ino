// RPWM에 듀티 → 정방향, LPWM에 듀티 → 역방향, 둘 다 0 → 정지, 둘 다 듀티 -> Brake
// RPWM, LPWM이 둘 다 켜지면 급속 BRAKE 가 걸린다.

void liner_motor_Init() {
    // 마이크로 스위치가 눌릴 시 LINER MOTOR가 멈춰야한다.
    pinMode(LINER_MOTOR_STOP_SWITCH, INPUT); // GPIO36은 input-only 핀이라서 외부 풀다운 저항을 사용 중

    ledcAttach(LINER_RPWM_PIN, MOTOR_FREQ, MOTOR_RESOLUTION);
    ledcAttach(LINER_LPWM_PIN, MOTOR_FREQ, MOTOR_RESOLUTION);
    ledcWrite(LINER_RPWM_PIN, 0);  // 부팅 시 모터 정지 상태로 시작
    ledcWrite(LINER_LPWM_PIN, 0);
}

bool isLinerMotorStopSwitchPressed() {
    return digitalRead(LINER_MOTOR_STOP_SWITCH) == HIGH;  // 외부 풀다운: 평소 LOW, 눌리면 HIGH
}

void boxOpen() {
    Log("MOTOR", "box opening (" + String(BOX_OPEN_TIME / 1000) + "s)");
    ledcWrite(LINER_RPWM_PIN, 0);           // 반대쪽을 먼저 끄고 나서 듀티 출력
    ledcWrite(LINER_LPWM_PIN, MOTOR_SPEED);
    boxOpenStartTime = millis();            // 시작 시각 기록 → boxUpdate()가 4초 후 정지시킴
    boxState = BOX_OPENING;
}

void boxClose() {
    // 마이크로 스위치가 눌려있으면 이미 박스가 닫혀있다
    if (isLinerMotorStopSwitchPressed()) { Log("MOTOR", "close skip: already closed"); return; }

    Log("MOTOR", "box closing (until switch)");
    ledcWrite(LINER_LPWM_PIN, 0);
    ledcWrite(LINER_RPWM_PIN, MOTOR_SPEED);
    boxState = BOX_CLOSING;                 // boxUpdate()가 스위치 눌림을 감지하면 정지시킴
}

void liner_motor_stop() {
    ledcWrite(LINER_RPWM_PIN, 0);
    ledcWrite(LINER_LPWM_PIN, 0);
    boxState = BOX_IDLE;
}

// loop()에서 매번 호출할 것. 블로킹 없이 모터 정지 시점을 감시한다.
void boxUpdate() {
    if (boxState == BOX_IDLE) return;

    // 4초동안 모터를 정방향으로 구동 시킨 후 MOTOR_IDLE 상태로 바뀐다.
    if (boxState == BOX_OPENING && millis() - boxOpenStartTime >= BOX_OPEN_TIME) {
        liner_motor_stop();
        Log("MOTOR", "box open done");
    }
    // 마이크로 스위치가 눌릴 때까지 모터를 구동 시킨 후 IDLE 상태로 바뀐다.
    // 모터 노이즈로 인한 순간 HIGH 스파이크를 거르기 위해 SWITCH_DEBOUNCE_TIME(50ms) 연속 HIGH여야 눌림으로 인정
    else if (boxState == BOX_CLOSING) {
        if (isLinerMotorStopSwitchPressed()) {
            if (switchHighSince == 0) switchHighSince = millis();               // HIGH 시작 시각 기록
            else if (millis() - switchHighSince >= SWITCH_DEBOUNCE_TIME) {      // 연속 HIGH 유지 확인
                switchHighSince = 0;
                liner_motor_stop();
                Log("MOTOR", "box close done");
            }
        } else {
            switchHighSince = 0;   // HIGH가 끊기면(노이즈였으면) 다시 처음부터
        }
    }
}
