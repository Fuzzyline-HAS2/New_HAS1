void StepMotorInit(){
    pinMode(STEP_PIN, OUTPUT);
    pinMode(DIR_PIN, OUTPUT);
    pinMode(SW_PIN, INPUT_PULLUP);
}

void EscapeClose(){
    digitalWrite(DIR_PIN, LOW); // 모터 역방향
    digitalWrite(RELAY_PIN, HIGH); // 모터 전원 ON (HIGH=ON, LOW=OFF)
    Serial.println("Escapse Close");
    Serial.println("[DEBUG] SW_PIN initial=" + String(digitalRead(SW_PIN)));

    // SW_PIN 극성: 평소 LOW(0), 리미트 스위치가 눌리면 HIGH(1).
    // 따라서 "아직 안 눌림(LOW)"인 동안 모터를 돌리고, 눌리면(HIGH) 멈춘다.
    //
    // 실측 2026-09-13 (커밋 80bd9d6의 [SWDEBUG] 로그, 손으로 눌렀다 떼며 확인):
    //     평소      SW_PIN=0
    //     누름      SW_PIN=1
    //     뗌        SW_PIN=0
    //
    // 삭제된 QC/QC_Rules.h(커밋 967799f)는 "NO + INPUT_PULLUP, 안 눌림=HIGH,
    // 눌림=LOW (실측 확인됨)"이라고 기술했으나 실제 하드웨어와 반대였다. 그 문서를
    // 근거로 조건을 == HIGH 로 두는 바람에, 평소 LOW인 상태에서 while이 한 번도
    // 실행되지 않아 모터가 아예 돌지 않았고, 스위치를 누르고 있으면 오히려 계속
    // 돌았다 — 현장 리포트 "마이크로스위치 다 눌려도 모터 안 멈춤"과 일치한다.
    //
    // 이 핀 극성은 3c990e1 / 8a12233 / 4a28456 / e221dec 에서 반복해서 뒤집혔다.
    // 이번에는 문서가 아니라 기기 실측을 근거로 한다.
    //
    // 안전장치: 배선/접점 불량 등으로 SW_PIN이 끝내 HIGH로 전환되지 않는
    // 경우를 대비해 최대 스텝 수를 넘으면 강제 종료한다.
    const unsigned long maxSteps = (unsigned long)stepsPerRevolution * 15;
    unsigned long stepCount = 0;
    while(digitalRead(SW_PIN) == LOW && stepCount < maxSteps)
    {
        if (stepCount % 200 == 0) {
            Serial.println("[DEBUG] step=" + String(stepCount) + " SW_PIN=" + String(digitalRead(SW_PIN)));
        }
        digitalWrite(STEP_PIN, LOW);
        delayMicroseconds(2000);
        digitalWrite(STEP_PIN, HIGH);
        delayMicroseconds(2000);
        stepCount++;
    }
    if (stepCount >= maxSteps) {
        Serial.println("[WARN] EscapeClose: SW_PIN 트리거 없이 최대 스텝(" + String(maxSteps) +
                       ") 도달, 강제 종료. SW_PIN=" + String(digitalRead(SW_PIN)));
    } else {
        Serial.println("[DEBUG] SW_PIN triggered at step=" + String(stepCount));
    }
    digitalWrite(RELAY_PIN, LOW); // 닫힘 완료 후 모터 전원 OFF
    Serial.println("Close Finish");
}

void EscapeOpen(){
    digitalWrite(DIR_PIN, HIGH); // 모터 정방향
    digitalWrite(RELAY_PIN, LOW);
    Serial.println("Escapse Open");

    for(int x = 0; x < (stepsPerRevolution*10); x++)
    {
        digitalWrite(STEP_PIN, LOW);
        delayMicroseconds(2000);
        digitalWrite(STEP_PIN, HIGH);
        delayMicroseconds(2000);
    }
    Serial.println("Open Finish");
}
