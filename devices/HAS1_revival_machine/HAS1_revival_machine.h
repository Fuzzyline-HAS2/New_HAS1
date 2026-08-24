#ifndef _HAS1_REVIVAL_MACHINE_H_
#define _HAS1_REVIVAL_MACHINE_H_

#include "library_and_pin.h"
#include "location_protocol.h"

// Telnet 원격 디버깅 콘솔 — Serial을 텔넷으로 미러링 (telnet.ino 구현).
// 아래 #define으로 기존 코드 전체의 Serial.print/println/printf 호출이 자동으로
// USB 시리얼 + Telnet 클라이언트 양쪽에 동시 출력된다 (기존 호출부는 수정 불필요).
class TelnetDebugConsole : public Stream {
public:
  void begin(unsigned long baud);
  int available() override;
  int read() override;
  int peek() override;
  void flush() override;
  size_t write(uint8_t data) override;
  size_t write(const uint8_t *buffer, size_t size) override;
};
extern HardwareSerial HardwareDebugSerial;
extern TelnetDebugConsole DebugSerial;
#define Serial DebugSerial

void TelnetInit(); // Telnet 서버 시작 — WiFi 연결 완료 후 호출 (telnet.ino)
void TelnetRun();  // 클라이언트 접속/데이터 처리 — loop()에서 매 프레임 호출 (telnet.ino)

//============================ Global Variable ============================
void NeoNo();
void (*NeoFunc)() = NeoNo;

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

// 근접 인식 Dead Zone 대응용 RxGain 전환 (rfid.ino 구현) — GainMode는 currentGain 등
// 내부 상태 변수 타입으로만 쓰이고 함수 매개변수 타입으로는 쓰이지 않는다(ApplyGain은 int를 받음).
// Arduino가 .ino 탭들을 병합할 때 자동 생성하는 함수 프로토타입이 실제 코드보다도 앞에
// 삽입돼서, 커스텀 enum을 매개변수로 쓰면 "타입을 아직 모른다"는 컴파일 에러가 나기 때문.
enum GainMode { GAIN_NEAR, GAIN_FAR };

void RfidInit(void);
void RfidLoop(void);
void CardChecking(uint8_t rfidData[32]);

//=============================== Neopixel ===============================
#define NUMPIXELS_TOP 24
#define NUMPIXELS_MID 16
#define NUMPIXELS_BOT 16
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
void NeoBlinkPurple(int times);  // tagger 상태에서 태그 시 "사용 불가" 알림으로 보라색 점멸

//=============================== Solenoid ================================
void SolenoidInit();
void SolenoidOn();
void SolenoidOff();
void SolenoidPulse();  // 상태 전환 순간에만 SOLENOID_PULSE_MS만큼 짧게 통전 후 자동으로 끔
void SolenoidPulse(unsigned long ms);  // 지정한 ms만큼 통전 후 자동으로 끔

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

//=========================== Open 잠금 ===========================
// ghost 태그로 열림이 로컬에서 커밋된 순간 즉시 잠그는 로컬 래치.
// device_state="open" 은 서버 왕복 후에야 반영되는데, 그 사이(서버 반영 지연 구간)에
// ghost 태그를 다시 대면 device_state 가 아직 "open" 이 아니라 가드를 통과해 중복 전송이 발생한다.
// 이 래치로 그 구간을 막고, device_state 가 "activate" 로 (재)전환될 때 해제한다.
bool ghost_opened_local = false;

void TimerInit();
void TimerRun();
void RfidTimerAssess();
void RfidTagTimerFunc();
void WifiTimerFunc();
void NsecTagTimerFailFunc();
void NsecTagTimerSuccessFunc();

#endif
