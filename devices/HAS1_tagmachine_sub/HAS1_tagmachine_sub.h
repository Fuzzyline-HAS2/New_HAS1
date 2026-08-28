#ifndef _HAS1_TAGMACHINE_SUB_
#define _HAS1_TAGMACHINE_SUB_

#include "library_and_pin.h"
const int rfid_num = 3; // 설치된 pn532의 개수

HardwareSerial fromSubSerial(1);
//****************************************SimpleTimer SETUP****************************************************************
SimpleTimer GameTimer;
void TimerInit();
void GameTimerFunc();
int gameTimerId;

//****************************************Pointer System****************************************************************
void (*ptrCurrentMode)();   //현재모드 저장용 포인터 함수

//****************************************RFID SETUP****************************************************************
Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS1);
void RfidInit(void);
void RfidLoopMain(void);
bool rfid_init_complete = false;

// 근접 인식 Dead Zone 대응 — RxGain 동적 전환 (rfid.ino 구현).
// GainMode는 반드시 여기(헤더)서 정의해야 한다 — Arduino가 .ino 탭들을 병합할 때 자동
// 생성하는 함수 프로토타입을 스케치 맨 앞(이 헤더 include 다음, 각 .ino의 실제 코드보다 앞)에
// 삽입하므로, rfid.ino 안에서만 정의하면 그 프로토타입 자리에서 "GainMode를 아직 모른다"는
// 컴파일 에러가 난다.
enum GainMode { GAIN_NEAR, GAIN_FAR };

// 유지 중이던 태그가 두 Gain 모두에서 이 시간 이상 연속으로 안 잡히면 그제서야 제거로 판정.
// (단 한 번의 Read 실패로 바로 태그 제거 처리하지 않기 위한 디바운스 — 500~1000ms 범위에서 조정 가능)
#define TAG_REMOVE_TIME_MS 500
#endif


