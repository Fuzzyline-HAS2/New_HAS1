/**
 * @file HAS1_revival_machine.ino
 * @author YuBin Kim
 * @brief
 * @version 0.1
 * @date 2022-11-24 ~ 2022-11-26
 *
 * @copyright Copyright (c) 2022
 */

#define FIRMWARE_VER 29
#define PARTITION_VER 5
#include "HAS1_revival_machine.h"

//************************************************ Core1 ********************************************************************
/**
 * @brief Temple Intialize
 */
void TempleInit()
{
  has2wifi.Setup("badland");
  has2wifi.Send((String)(const char *)my["device_name"], "esp_version", String(FIRMWARE_VER));
  TelnetInit(); // Telnet 서버 시작 (WiFi 연결 완료 후) — 이후 Serial.* 출력은 telnet.ino로 미러링됨
  LogMemoryStats("Wi-Fi connected");
  BleAdvertiserInit();
  ota.setLogStream(Serial);
  ota.setOnSuccess([]() {
    has2wifi.Send((String)(const char *)my["device_name"], "device_state", "setting");
  });
  ota.setOnSkip([]() {
    has2wifi.Send((String)(const char *)my["device_name"], "device_state", "setting");
  });
    ota.setPartitionUpdate(
        "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_revival_machine/partitions.bin",
        "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_revival_machine/partitions.sig",
        "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_revival_machine/partition_version.txt",
        PARTITION_VER
    );
  SensorInit();  // IoT Glove 사용 센서, 모듈 세팅
  TimerInit();   // 타이머 세팅
  
}

/**
 * @brief 아두이노 기본 문법 (전원이 켜지면 한번만 실행)
 */
void setup()
{
  delay(1000);
  Serial.begin(115200);
  LogMemoryStats("boot");
  TempleInit();
  DataChange();
}

/**
 * @brief 아두이노 기본 문법 (전원이 켜져있는동안 Core1에서 계속 실행)
 */
void loop()
{
  TelnetRun(); // Telnet 클라이언트 접속/데이터 처리
  TimerRun();
  NeoFunc();
  if (activate_bool)
  {
    ActivateFunc();
  }
}
