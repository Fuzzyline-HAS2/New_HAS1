#include "HAS1_itembox.h"

void setup() {
  Serial.begin(115200);
  Mp3_Init();
  EncoderInit();
  liner_motor_Init();
  vibration_motor_Init();
  NeopixelInit();
  RfidInit();
}

void loop() {
  boxUpdate();   // 리니어 모터 정지 감시 (4초 열림 / 스위치 닫힘)
  GameUpdate();  // 게임 상태 머신
}
