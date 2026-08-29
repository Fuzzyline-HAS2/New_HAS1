#ifndef _HAS1_ALTAR_H_
#define _HAS1_ALTAR_H_

#include "library_and_pin.h"
#include "location_protocol.h"

//============================ Global Variable ============================
void NeoNo();
void (*NeoFunc)() = NeoNo;

//================================ Wifi ==================================
void TelnetInit();
void TelnetLoop();

HAS2_Wifi has2wifi("http://172.30.1.43");

SecureOTA ota(
  "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_altar/update.bin",
  "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_altar/version.txt",
  "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_altar/update.sig",
  HMAC_SECRET,
  FIRMWARE_VER
);

bool activate_bool;

void SettingFunc();
void ReadyFunc();
void ActionFunc();
void DataChange();
void CrashReportInit();
void CrashReportSend(const char *device_name);

//* =============================== Sensor =============================== *
/**
 * @brief Temple에 사용되는 센서, 모듈 세팅
 */
void SensorInit();
void BleAdvertiserInit();
void BleAdvertiserUpdateFromDeviceName(const char *device_name);
void BleAdvertiserMaintain();
void LogMemoryStats(const char *stage);

//=============================== Solenoid ================================
void SolenoidInit();
void SolenoidOn();
void SolenoidOff();
void SolenoidPulse();
void SolenoidPulse(unsigned long ms);

//============================== IR Sensor =================================
// 생명칩이 투입구를 통과하면 감지 (device 로직: sensor.ino)
void IrSensorInit();
void IrSensorLoop();

//============================= Micro Switch ================================
// 회전 메커니즘이 한바퀴 돌면 눌림 → 솔레노이드 개방 트리거 (sensor.ino)
void MicroSwInit();
void MicroSwLoop();

//=============================== DFPlayer (MP3) ============================
void Mp3_Init();
void Mp3PlayLargeFolder(uint8_t folder_number, uint16_t file_number);

//================================ RFID ==================================
Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);

bool rfid_tag = false;
byte rfid_tag_count = 0; // 몇번 태그 됐는지 (= 덕트를 몇 번 사용했는지) 확인하는 변수

bool send_nfc_err = false;

void RfidInit(void);
void RfidLoop(void);
void CardChecking(uint8_t rfidData[32]);
void RunAltarSuccess(); // 태그+칩이 확인됐을 때 공통 처리 (순서 무관하게 CardChecking/IrSensorLoop에서 호출)

//=============================== Neopixel ===============================
// TODO 네오픽셀 개수 확인
#define NUMPIXELS_SQUARE 15   // NEO_SQUARE1
#define NUMPIXELS_ROUND 24
#define NUMPIXELS_SIDE 96
#define NUMPIXELS_SQUARE2 15  // NEO_SQUARE2 — 실측 전까지 추정치
#define NUMPIXELS_PN532   8   // NEO_PN532 — 실측 전까지 추정치
Adafruit_NeoPixel pixels_square(NUMPIXELS_SQUARE, NEOPIXEL_PIN_SQUARE, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel pixels_round(NUMPIXELS_ROUND, NEOPIXEL_PIN_ROUND, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel pixels_side(NUMPIXELS_SIDE, NEOPIXEL_PIN_SIDE, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel pixels_square2(NUMPIXELS_SQUARE2, NEOPIXEL_PIN_SQUARE2, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel pixels_pn532(NUMPIXELS_PN532, NEOPIXEL_PIN_PN532, NEO_GRB + NEO_KHZ800);

// Neopixel 색상정보
int black[3] = {0, 0, 0};
int white[3] = {20, 20, 20};
int red[3] = {20, 0, 0};
int yellow[3] = {20, 20, 0};
int green[3] = {0, 20, 0};
int purple[3] = {20, 0, 20};
int purple_white[3] = {20, 10, 20}; // purple + white 중간톤 (blink 진입 시 side용)

void applyBrightness();
void lightColor(Adafruit_NeoPixel &pixels, int color[3]);
void lightColor(Adafruit_NeoPixel &pixels, int color[3], int index);
void lightRgb(Adafruit_NeoPixel &pixels, int r, int g, int b);
void ClearSquare();  // pixels_square + pixels_square2(둘째 사각 링) 동시 클리어
void ClearRound();   // pixels_round + pixels_pn532(태그 지점 링) 동시 클리어

void NeoBeforeTagger();
void NeoTagger();
void NeoTaggerTag();
void NeoAfterTagger();
void NeoGaming();
void NeoChipGaugeBlink();      // blink: 생명칩 태그 시 round+square가 함께 차오르는 게이지
void NeoChipBlinkActivate();   // activate: 생명칩 태그(+1) 시 round+square가 깜빡임
void NeoTakenChip();
void NeoWin();
void NeoLose();

//================================ Timer =================================
// 1초마다 RFID 가 인식되게 타이머 설정
SimpleTimer rfid_timer;
SimpleTimer nsec_tag_timer;
SimpleTimer wifi_timer;
SimpleTimer solenoid_timer;  // SolenoidPulse()를 논블로킹으로 닫기 위한 타이머

int rfid_timer_id;
int nsec_tag_timer_id;
int wifi_timer_id;
int solenoid_timer_id;

int nsec_tag_num;
bool nsec_tag_bool;

void TimerInit();
void TimerRun();
void RfidTimerAssess();
void RfidTagTimerFunc();
void WifiTimerFunc();
void NsecTagTimerFailFunc();
void NsecTagTimerSuccessFunc();

#endif