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

#define FIRMWARE_VER 23
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

    // QC 엔진 초기화 및 룰 등록
    // QCEngine& qc = QCEngine::getInstance();
    // qc.begin(2000);
    // qc.addRule(new QCRule_HeapMemory());
    // qc.addRule(new QCRule_WifiConnection());
    // qc.addRule(new QCRule_WifiSignal());
    // qc.addRule(new QCRule_ResetReason());
    // qc.addRule(new QCRule_PinConflict());
    // qc.addRule(new QCRule_GpioCapability());
    // qc.addRule(new QCRule_GameState());
    // qc.addRule(new QCRule_TagCntBounds());
    // qc.addRule(new QCRule_StepperConfig());
    // qc.addRule(new QCRule_LimitSwitch());
    // qc.addRule(new QCRule_RelayState());
    // qc.addRule(new QCRule_SwitchChatter());      // HW_SW_03
    // qc.addRule(new QCRule_BeetlePacketFormat()); // LOGIC_SERIAL_02
    // qc.addRule(new QCRule_InvalidCommand());     // LOGIC_SERIAL_03
    // qc.addRule(new QCRule_TagParseError());      // LOGIC_TAG_02
    // qc.addRule(new QCRule_StrappingPins());      // HW_BOOT_01
    // qc.addRule(new QCRule_OutputInitSafety());   // HW_GPIO_02
}
void loop() {
    TelnetRun();
    WifiTimer.run();
    GameTimer.run();
    //QCEngine::getInstance().tick();
}
