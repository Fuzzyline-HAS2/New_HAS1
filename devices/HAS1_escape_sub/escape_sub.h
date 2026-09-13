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

// T 패킷은 "지금 각 리더에 뭐가 올려져 있나"의 스냅샷이고, TTGO는 폴링 때 가장 최근 것
// 하나만 쓴다. 예전에는 loop()에 딜레이가 없어 스캔 속도대로(초당 수십 줄) 내보냈는데,
// 그 대부분은 버려지려고 만들어진 것이었고 TTGO의 RX 버퍼를 넘치게 해서 줄이 잘렸다.
// 잘린 구간에 1회성 이벤트인 'M'이 걸리면 MMMM 태그가 통째로 유실된다.
// 값이 바뀔 때만 보낸다. 스캔 자체는 그대로 전속력이므로 새 태그는 "변화"라서 즉시 나간다.
// 변화가 없어도 하트비트 주기로 한 번은 보내 TTGO가 현재 상태를 잃지 않게 한다.
String lastSentPacket = "";
unsigned long lastSentMs = 0;
const unsigned long PACKET_HEARTBEAT_MS = 1000;
bool mmmmPresent = false;      // 이번 스캔에서 MMMM이 보였나
bool mmmmPresentPrev = false;  // 직전 스캔에서 보였나 (상승 에지 판정용)
void RfidInit(void);
void RfidLoopMain(void);
#endif


