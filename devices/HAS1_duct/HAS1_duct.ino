/**
 * @file updated_duct.ino
 * @author YuBin Kim (dbqks56231106@gmail.com)
 * @brief badland 덕트
 * @version 0.0.1
 * @date 2023-02-03
 * 
 * @copyright Copyright (c) 2023
 * 
 */

#define FIRMWARE_VER 34
#define PARTITION_VER 1
#include "HAS1_duct.h"

//************************************************ Core1 ********************************************************************
/**
 * @brief  Duct Initialize
 */
void DuctInit() {
  Serial.begin(115200);
  has2wifi.Setup("city");
  // 부팅 시 현재 펌웨어 버전을 서버(esp_version)로 전송
  has2wifi.Send((String)(const char*)my["device_name"], "esp_version", String(FIRMWARE_VER));
  ota.setLogStream(Serial);
  ota.setOnSuccess([]() {
    has2wifi.Send((String)(const char*)my["device_name"], "device_state", "setting");
  });
  ota.setOnSkip([]() {
    has2wifi.Send((String)(const char*)my["device_name"], "device_state", "setting");
  });
  ota.setPartitionUpdate(
    "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_duct/partitions.bin",
    "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_duct/partitions.sig",
    "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_duct/partition_version.txt",
    PARTITION_VER);
  SensorInit();
  DataChange();
  TimerInit();
}

/**
 * @brief 아두이노 기본 문법 (전원이 켜지면 한번만 실행)
 */
void setup() {
  delay(1000);
  Serial.begin(115200);
  DuctInit();
}

/**
 * @brief 아두이노 기본 문법 (전원이 켜져있는동안 Core1에서 계속 실행)
 */
void loop() {
  TimerRun();
  EmegencyPush();

  if (game_state == activate) {
    ActivateFunc();
  } else {  // 쿨타임 없이 사용(크루용) 세팅 모드 : 태그, 스위치 둘다 가능 / 레디 : 스위치만 가능
    RfidLoop();
    if (!digitalRead(SW_PIN)) {
      Serial.println("staff");
      digitalWrite(RELAY_PIN, HIGH);
      delay(5000);
      digitalWrite(RELAY_PIN, LOW);
    }
  }
}