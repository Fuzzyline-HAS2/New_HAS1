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

    // 안전장치: 배선/접점 불량 등으로 SW_PIN이 끝내 HIGH로 전환되지 않는
    // 경우를 대비해 최대 스텝 수를 넘으면 강제 종료한다.
    const unsigned long maxSteps = (unsigned long)stepsPerRevolution * 15;
    unsigned long stepCount = 0;
    while(digitalRead(SW_PIN) == HIGH && stepCount < maxSteps)
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
