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

#define FIRMWARE_VER 36
#define PARTITION_VER 1
#include "HAS1_escape_main.h"

void setup() {
    delay(500);
    Serial.begin(115200);
    // 기본 RX 버퍼는 256바이트 = T 패킷(25바이트) 약 10개분인데, 모터가 도는 4초 동안
    // Beetle이 계속 보내므로 넘칠 수 있다. 방어적으로 1024로 늘린다.
    // 주의: 이걸로 "[UART] WARN unknown command" 경고가 없어지지는 않는다. 실측 결과
    // 버퍼를 늘려도 그대로 남았고, 경고 문자가 '1' ':' 'x' 'P' '0'처럼 전부 T 패킷
    // 중간 글자였다. DrainSubSerial()이 available()==0에서 멈추느라 전송 도중에 끊고,
    // 다음 읽기가 그 줄의 꼬리부터 집는 것이 원인이다 (별건).
    // setRxBufferSize()는 begin() 이전에 불러야 적용된다.
    toSubSerial.setRxBufferSize(1024);
    toSubSerial.begin(115200, SERIAL_8N1, HWSERIAL_RX, HWSERIAL_TX);
//    has2wifi.Setup("city");
    has2wifi.Setup("badland");
    // 부팅 시 현재 펌웨어 버전을 서버(esp_version)에 한 번 보고한다.
    has2wifi.Send((String)(const char*)my["device_name"], "esp_version", String(FIRMWARE_VER));
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
    // WiFi와 모터/Beetle 초기화가 끝난 뒤에만 BLE 컨트롤러를 준비한다.
    Has1BleBeacon::begin();
}
void loop() {
    TelnetRun();
    WifiTimer.run();
    GameTimer.run();
    BeetleTimer.run();

    // 진단용: 모터와 무관하게 SW_PIN 값을 500ms마다 출력.
    // 손으로 마이크로스위치를 눌러보면서 값이 바뀌는지 확인하기 위함.
    static unsigned long lastSwDebugMs = 0;
    if (millis() - lastSwDebugMs >= 500) {
        lastSwDebugMs = millis();
        Serial.println("[SWDEBUG] SW_PIN=" + String(digitalRead(SW_PIN)));
    }
    // 모터 펄스 루프 안에서는 호출하지 않는다. 모든 장치 처리 뒤 한 단계만 진행한다.
    Has1BleBeacon::poll();
}
