#include "motor_hal.h"
#include "library_and_pin.h"

#define MOTOR_FREQ       5000
#define MOTOR_RESOLUTION 8
#define MOTOR_SPEED      255
#define BOX_OPEN_TIME    4000

enum BoxState { BOX_IDLE, BOX_OPENING, BOX_CLOSING };
BoxState      boxState        = BOX_IDLE;
unsigned long boxOpenStartTime = 0;
unsigned long switchHighSince  = 0;
const unsigned long SWITCH_DEBOUNCE_TIME = 50;

// RPWM에 듀티 → 정방향, LPWM에 듀티 → 역방향, 둘 다 0 → 정지
void MotorInit() {
    pinMode(LINER_MOTOR_STOP_SWITCH, INPUT);
    ledcAttach(LINER_RPWM_PIN, MOTOR_FREQ, MOTOR_RESOLUTION);
    ledcAttach(LINER_LPWM_PIN, MOTOR_FREQ, MOTOR_RESOLUTION);
    ledcWrite(LINER_RPWM_PIN, 0);
    ledcWrite(LINER_LPWM_PIN, 0);
}

static bool isSwitchPressed() {
    return digitalRead(LINER_MOTOR_STOP_SWITCH) == HIGH;  // 외부 풀다운: 평소 LOW, 눌리면 HIGH
}

static void liner_motor_stop() {
    ledcWrite(LINER_RPWM_PIN, 0);
    ledcWrite(LINER_LPWM_PIN, 0);
    boxState = BOX_IDLE;
}

bool isBoxOpened() {
    // BOX_IDLE 이고 스위치가 눌리지 않으면 박스가 열린 상태 (스위치 눌림 = 닫힘 완료)
    return boxState == BOX_IDLE && !isSwitchPressed();
}

void boxOpen() {
    Log("MOTOR", "box opening (" + String(BOX_OPEN_TIME / 1000) + "s)");
    ledcWrite(LINER_RPWM_PIN, 0);
    ledcWrite(LINER_LPWM_PIN, MOTOR_SPEED);
    boxOpenStartTime = millis();
    boxState = BOX_OPENING;
}

void boxClose() {
    if (isSwitchPressed()) { Log("MOTOR", "close skip: already closed"); return; }
    Log("MOTOR", "box closing (until switch)");
    ledcWrite(LINER_LPWM_PIN, 0);
    ledcWrite(LINER_RPWM_PIN, MOTOR_SPEED);
    boxState = BOX_CLOSING;
}

void MotorHalUpdate() {
    if (boxState == BOX_IDLE) return;

    if (boxState == BOX_OPENING && millis() - boxOpenStartTime >= BOX_OPEN_TIME) {
        liner_motor_stop();
        Log("MOTOR", "box open done");
    }
    else if (boxState == BOX_CLOSING) {
        if (isSwitchPressed()) {
            if (switchHighSince == 0) switchHighSince = millis();
            else if (millis() - switchHighSince >= SWITCH_DEBOUNCE_TIME) {
                switchHighSince = 0;
                liner_motor_stop();
                Log("MOTOR", "box close done");
            }
        } else {
            switchHighSince = 0;  // 노이즈 스파이크면 다시 처음부터
        }
    }
}
