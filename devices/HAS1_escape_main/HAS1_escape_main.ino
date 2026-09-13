 /**
 * @file Done_Escape_Main_code.ino
 * @author 김병준 (you@domain.com)
 * @brief
 * @version 1.0
 * @date 2022-11-29
 *
 * @copyright Copyright (c) 2022
 *
 */

#define FIRMWARE_VER 33
#define PARTITION_VER 1
#include "HAS1_escape_main.h"

void setup() {
    delay(500);
    Serial.begin(115200);
    toSubSerial.begin(115200, SERIAL_8N1, HWSERIAL_RX, HWSERIAL_TX);
//    has2wifi.Setup("city");
    has2wifi.Setup("badland");
    ota.setLogStream(DebugSerial);
    ota.setOnSuccess([]() {
        ClearGithubOtaState();
    });
    ota.setOnSkip([]() {
        ClearGithubOtaState();
    });
    ota.setPartitionUpdate(
        "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_escape_main/partitions.bin",
        "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_escape_main/partitions.sig",
        "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_escape_main/partition_version.txt",
        PARTITION_VER
    );
    TelnetInit();
    NeopixelInit();
    TimerInit();
    Mp3_Setup();
    StepMotorInit();
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, HIGH);
    // has2wifi.Setup("KT_GiGA_6C64","ed46zx1198");
    // has2wifi.Setup();
    DataChanged();
    toSubSerial.println("R");
    toSubSerial.println("R");
    toSubSerial.println("R");
}
void loop() {
    TelnetRun();
    WifiTimer.run();
    GameTimer.run();

    // 진단용: 모터와 무관하게 SW_PIN 값을 500ms마다 출력.
    // 손으로 마이크로스위치를 눌러보면서 값이 바뀌는지 확인하기 위함.
    static unsigned long lastSwDebugMs = 0;
    if (millis() - lastSwDebugMs >= 500) {
        lastSwDebugMs = millis();
        Serial.println("[SWDEBUG] SW_PIN=" + String(digitalRead(SW_PIN)));
    }
}
