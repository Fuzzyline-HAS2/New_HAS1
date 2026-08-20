void StepMotorInit(){
    pinMode(STEP_PIN, OUTPUT);
    pinMode(DIR_PIN, OUTPUT);
    pinMode(SW_PIN, INPUT_PULLUP);
}

void EscapeClose(){
    digitalWrite(DIR_PIN, LOW); // 모터 역방향
    digitalWrite(RELAY_PIN, HIGH);
    Serial.println("Escapse Close");

    while(digitalRead(SW_PIN) == LOW)
    {
        digitalWrite(STEP_PIN, LOW);
        delayMicroseconds(2000);
        digitalWrite(STEP_PIN, HIGH);
        delayMicroseconds(2000);
    }
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
