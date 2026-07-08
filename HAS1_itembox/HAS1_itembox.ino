#include "HAS1_itembox.h"

void setup() {
  Serial.begin(115200);
  Log("MAIN", "boot");
  vibration_motor_Init();  // GPIO14는 부팅 중 잠깐 HIGH가 떠서 진동이 울림 → 가장 먼저 0으로 잡아준다
  liner_motor_Init();      // 모터류 핀을 먼저 안전 상태로
  EncoderInit();
  NeopixelInit();
  RfidInit();
  TelnetInit();            // WiFi 연결 시도 시작 (논블로킹 — Mp3_Init의 2초 대기 동안 백그라운드로 붙음)
  Mp3_Init();              // 내부에 2초 대기가 있어서 맨 마지막
  Log("MAIN", "setup done");
}

void loop() {
  boxUpdate();   // 리니어 모터 정지 감시 (4초 열림 / 스위치 닫힘)
  GameUpdate();  // 게임 상태 머신
  TelnetRun();   // 텔넷 클라이언트 접속 관리 (로그 미러링은 Log()가 담당)
}
