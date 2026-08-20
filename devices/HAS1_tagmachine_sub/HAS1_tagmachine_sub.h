#ifndef _HAS1_TAGMACHINE_SUB_
#define _HAS1_TAGMACHINE_SUB_

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

//****************************************RFID SETUP****************************************************************
Adafruit_PN532_Debug nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS1);
void RfidInit(void);
void RfidLoopMain(void);
bool rfid_init_complete = false;

// PN532 근접 인식 Dead Zone 대응용 RxGain 전환 (rfid.ino 구현) — GainMode는
// currentGain의 타입으로만 쓰이고 함수 매개변수 타입으로는 쓰지 않는다(ApplyGain은 int를 받음).
// Arduino가 .ino 탭들을 병합할 때 자동 생성하는 함수 프로토타입이 실제 코드보다도 앞에
// 삽입돼서, 커스텀 enum을 매개변수로 쓰면 "타입을 아직 모른다"는 컴파일 에러가 나기 때문.
enum GainMode { GAIN_NEAR, GAIN_FAR };
bool ApplyGain(int mode);

//****************************************OTA (TTGO 경유 UART 릴레이) SETUP****************************************************************
// Beetle(ESP32-C3)은 자체 WiFi가 없어 GitHub에서 직접 OTA를 받을 수 없다. 대신 TTGO가
// GitHub Release(HAS1_tagmachine_sub 태그)에서 다운로드 + HMAC-SHA256 서명 검증까지 마친
// 펌웨어를 이 UART로 그대로 릴레이해주면, 여기서는 받은 바이트를 Update.h에 흘려 넣기만
// 한다(서명 재검증은 하지 않음 — 물리적으로 기기 내부에 유선 연결된 사설 링크라 TTGO를 신뢰).
// 반드시 HAS1_tagmachine_main/beetle_ota.ino의 정의와 동기화해서 유지할 것.
// (SOF/TYPE 상수를 ota.ino가 아니라 여기 두는 이유: Arduino는 .ino 탭들을 병합할 때
//  메인 스케치 파일을 가장 앞에, 나머지 탭은 알파벳 순으로 그 뒤에 붙이므로 — Game_system.ino,
//  ota.ino, rfid.ino, timer.ino 순 — HAS1_tagmachine_sub.ino의 loop()가 참조하는 OTA_SOF1이
//  ota.ino의 #define보다 먼저 나와 컴파일 에러가 난다. #define은 함수와 달리 자동
//  프로토타입 생성 대상이 아니라서 반드시 먼저 병합되는 헤더에 있어야 한다.)
#define OTA_SOF1 0xA5
#define OTA_SOF2 0x5A

#define OTA_TYPE_BEGIN 0x01  // TTGO->Beetle, payload: uint32 total_size(LE)
#define OTA_TYPE_DATA  0x02  // TTGO->Beetle, payload: 펌웨어 조각
#define OTA_TYPE_END   0x03  // TTGO->Beetle, payload 없음
#define OTA_TYPE_ABORT 0x04  // TTGO->Beetle, payload 없음
#define OTA_TYPE_ACK   0x10  // Beetle->TTGO, payload 없음
#define OTA_TYPE_NAK   0x11  // Beetle->TTGO, payload: 1바이트 사유 코드

#define OTA_MAX_PAYLOAD 512          // HAS1_tagmachine_main/beetle_ota.ino와 반드시 동일해야 함
#define OTA_FRAME_TIMEOUT_MS 5000    // 프레임 한 조각(바이트열)을 기다리는 최대 시간

void HandleOtaSession();
#endif


