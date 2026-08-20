#ifndef _HAS1_REVIVAL_MACHINE_H_
#define _HAS1_REVIVAL_MACHINE_H_

#include "library_and_pin.h"
#include "location_protocol.h"

//============================ Global Variable ============================
void NeoNo();
void (*NeoFunc)() = NeoNo;

//============================ Hardware Serial ============================
// HardwareSerial MySerial1(1); // 사용X
HardwareSerial MySerial2(2); // Display

//================================ Wifi ==================================
HAS2_Wifi has2wifi("http://172.30.1.43");

SecureOTA ota(
  "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_revival_machine/update.bin",
  "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_revival_machine/version.txt",
  "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_revival_machine/update.sig",
  HMAC_SECRET,
  FIRMWARE_VER
);

bool activate_bool;

void SettingFunc();
void ReadyFunc();
void ActionFunc();
void DataChange();

//=============================== Display ================================
int nextion_language = 1;  // 0=EN, 1=KO (기본값 KO)
// pgUsed 페이지 id ("get dp" 응답 기준). HMI 변경 시 함께 갱신할 것.
#define PG_USED_ID 6
void DisplayCheck();
void NextionReceived(String *nextion_string);
int NextionGetNum(const char *cmd);
void NextionInit();
void SyncLanguage();
void nexInit();
void sendCommand(const char *cmd);

//* =============================== Sensor =============================== *
/**
 * @brief Temple에 사용되는 센서, 모듈 세팅
 */
void SensorInit();
void BleAdvertiserInit();
void BleAdvertiserUpdateFromDeviceName(const char *device_name);
void BleAdvertiserMaintain();
void LogMemoryStats(const char *stage);

//================================ RFID ==================================
Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);

bool rfid_tag = false;
byte rfid_tag_count = 0; // 몇번 태그 됐는지 (= 덕트를 몇 번 사용했는지) 확인하는 변수

bool send_nfc_err = false;

void RfidInit(void);
void RfidLoop(void);
void CardChecking(uint8_t rfidData[32]);

//=============================== Neopixel ===============================
#define NUMPIXELS_TOP 24
#define NUMPIXELS_MID 16
#define NUMPIXELS_BOT 8
Adafruit_NeoPixel pixels_top(NUMPIXELS_TOP, NEOPIXEL_TOP_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel pixels_mid(NUMPIXELS_MID, NEOPIXEL_MID_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel pixels_bot(NUMPIXELS_BOT, NEOPIXEL_BOT_PIN, NEO_GRB + NEO_KHZ800);

int arrow_neo_line_1;
int arrow_neo_line_2;
int arrow_neo_line_3;

// Neopixel 색상정보
// 밝기 제어 방식: 색상은 항상 풀 밝기(255)로 정의하고, 실제 밝기는
// Adafruit_NeoPixel::setBrightness()로 전역 스케일한다.
//   - DEFAULT_BRIGHTNESS : 서버 brightness(%) 미지정 시 기본 밝기 (0~255)
//   - 서버에서 brightness(1~100%)를 받으면 SetBrightness()가 1~255로 환산해 적용
#define DEFAULT_BRIGHTNESS 20   // 0~255. 이 값만 올리면 전체가 밝아진다.
int color_brightness = DEFAULT_BRIGHTNESS;

// breathe(숨쉬기) 애니메이션: 색 값을 0~BREATHE_MAX 로 왕복시켜 밝기 펄스를 만든다.
// 풀스케일(255)로 왕복하므로 최종 밝기는 setBrightness() 값에 비례한다.
#define BREATHE_MAX  255   // 펄스 최대 밝기 (풀스케일)
#define BREATHE_STEP 13    // 호출당 증감폭 (0→255 까지 약 20스텝 ≈ 2초)

// 색상은 풀 밝기(255). 실제 출력 밝기는 setBrightness()가 결정한다.
int white[3]  = {255, 255, 255};
int red[3]    = {255, 0,   0  };
int yellow[3] = {255, 255, 0  };
int green[3]  = {0,   255, 0  };
int purple[3] = {255, 0,   255};
int blue[3]   = {0,   0,   255};

int* current_neopixel_color = white;

void NeopixelSet(int color[3]);
void ApplyBrightness(int raw);
void SetBrightness(int pct);
void lightColor(Adafruit_NeoPixel &pixels, int color[3], int index);

void NeoBeforeTagger();
void NeoTagger();
void NeoTaggerTag();
void NeoAfterTagger();
void NeoGaming();
void NeoTakenChip();
void NeoWin();
void NeoLose();
void NeoArrow();
void NeoArrowSet(int arrow_neo_line_num, int arrow_neo_line);

//================================ Timer =================================
// 1초마다 RFID 가 인식되게 타이머 설정
SimpleTimer rfid_timer;
SimpleTimer nsec_tag_timer;
SimpleTimer wifi_timer;

int rfid_timer_id;
int nsec_tag_timer_id;
int wifi_timer_id;

int nsec_tag_num;
bool nsec_tag_bool;

//=========================== Used 잠금 ===========================
// 봉헌이 로컬에서 커밋된 순간 즉시 "사용됨"으로 잠그는 로컬 래치.
// device_state="used" 는 서버 왕복 후에야 반영되는데, Nextion 화면은 내부 타이머(3초)로
// 먼저 pgUsed 로 넘어간다. 그 사이(서버 반영 지연 구간)에 카드를 다시 대면 device_state 가
// 아직 "used" 가 아니라 가드를 통과해 pgKeepTag 로 넘어가고 중복 봉헌이 발생한다.
// 이 래치로 그 구간을 막고, device_state 가 "activate" 로 (재)전환될 때 해제한다.
bool altar_used_local = false;

void TimerInit();
void TimerRun();
void RfidTimerAssess();
void RfidTagTimerFunc();
void WifiTimerFunc();
void NsecTagTimerFailFunc();
void NsecTagTimerSuccessFunc();

#endif
