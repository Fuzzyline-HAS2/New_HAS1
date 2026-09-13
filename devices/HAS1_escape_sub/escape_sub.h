#ifndef _DONE_ITEMBOX_CODE_
#define _DONE_ITEMBOX_CODE_

#include "Library_and_pin.h"
const int rfid_num = 3; // 설치된 pn532의 개수

HardwareSerial fromSubSerial(1);
//****************************************SimpleTimer SETUP****************************************************************
SimpleTimer GameTimer;
void TimerInit();
void GameTimerFunc();
int gameTimerId;
//****************************************Pointer System****************************************************************
void (*ptrCurrentMode)();   //현재모드 저장용 포인터 함수

//****************************************Game System****************************************************************
HardwareSerial toMainSerial(2); //

//****************************************RFID SETUP****************************************************************
enum {PN532_A = 0, PN532_B, PN532_C};

// CS 핀만 지정 -> 하드웨어 SPI(HW 시프트레지스터) 사용. SCK/MISO/MOSI는 SPI.begin()에서 커스텀 핀으로 매핑 (setup() 참고).
// 기존 4핀(SCK,MISO,MOSI,SS) 생성자는 소프트웨어 비트뱅잉 SPI라 훨씬 느림.
Adafruit_PN532 nfc[3] = {Adafruit_PN532(PN532_SS1),
                         Adafruit_PN532(PN532_SS2),
                         Adafruit_PN532(PN532_SS3)};
bool rfid_init_complete[3];
int rfid_init_complete_cnt = 0;
struct TAGDATA{
    String tagData = "";
};
TAGDATA structTagData[3];

bool serialSend = false;
void RfidInit(void);
void RfidLoopMain(void);
#endif


