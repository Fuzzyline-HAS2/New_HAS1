#ifndef _UPDATED_DUCT_H_
#define _UPDATED_DUCT_H_

#include "library_and_pin.h"

// Telnet 원격 디버깅 콘솔 — Serial을 텔넷으로 미러링 (telnet.ino 구현, 다른 device들과 동일 패턴).
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
int use_duct_num;   // 덕트 사용횟수
bool duct_available = true;  // 덕트 사용 가능 
bool switch_available = true;
String tag_player_name = "";
String cur_tag_user = "";
int tagUser_tag_num = 0;

typedef enum GameState{ setting, ready, activate } GameState;

GameState game_state = setting;

bool cool_time_neo_bool = true;
bool tagger_mode = false;   // "이로운 효과"(덕트킬 포함) - tagger 수신 시 덕트 동결(보라색+RFID off), back 시 원복

bool tagger_blink_active = false;  // 봉쇄(tagger_mode) 중 태그 시 보라색 점멸(3회) 진행 여부
int tagger_blink_step = 0;

bool mmmm_open = false;
bool mmmm_prev_duct_available = false;
int mmmm_prev_current_time = 0;
bool mmmm_prev_cool_time_neo_bool = false;
//============================ Hardware Serial ============================
// HardwareSerial MySerial1(1); // 사용X
HardwareSerial MySerial2(2);    // MP3


//*================================ Duct ================================*
int current_time;
int cooltime;
int cooltime_set = 30;
int cooltime_add = 30;

void DuctTag(String tag_player);
void DuctOpen(bool switch_push = false);
void DuctClose();
void TaggerSwitchClose();
void CooltimeCalculation();
int  CooltimeBarPixels();
void CooltimeMp3();
void TagPlayerSend();
void DuctKill();
void TaggerModeTagBlocked();
void TaggerBlinkStep();
void TaggerSwitchBlocked();
void MmmmOpen();
void MmmmClose();

//*=============================== Sensor ===============================*
/**
 * @brief Duct에 사용되는 센서, 모듈 세팅
 */
void SensorInit();

//================================ Wifi ==================================
HAS2_Wifi has2wifi("http://172.30.1.43");

SecureOTA ota(
  "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_duct/update.bin",
  "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_duct/version.txt",
  "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_duct/update.sig",
  HMAC_SECRET,
  FIRMWARE_VER
);

bool activate_bool;

void UpdateBrightness();
void ApplyCurrentNeopixel();
void SettingFunc();
void ReadyFunc();
void ActionFunc();
void DataChange();
void EnterTaggerMode();
void ExitTaggerMode();

//=============================== Neopixel ===============================
#define NUMPIXELS_LINE           30
#define NUMPIXELS_ROUND          6
#define NUMPIXELS_SWITCH         12
#define DEFAULT_COLOR_BRIGHTNESS 50
#define DEFAULT_LINE_BRIGHTNESS  50

class NeoPixelExt : public Adafruit_NeoPixel {
public:
    NeoPixelExt(uint16_t n, int16_t pin, neoPixelType type) : Adafruit_NeoPixel(n, pin, type) {}

    void lightColor(int color[3]) {
        fill(Color(color[0], color[1], color[2]));
        show();
    }

    void lightColor(int color[3], int count) {
        clear();
        for (int i = 0; i < count; i++) {
            setPixelColor(i, Color(color[0], color[1], color[2]));
        }
        show();
    }
};

NeoPixelExt pixels_line(NUMPIXELS_LINE, NEO_LINE, NEO_GRB + NEO_KHZ800);
NeoPixelExt pixels_round(NUMPIXELS_ROUND, NEO_ROUND, NEO_GRB + NEO_KHZ800);
NeoPixelExt pixels_switch(NUMPIXELS_SWITCH, NEO_SWITCH, NEO_GRB + NEO_KHZ800);

int colorBrightness = DEFAULT_COLOR_BRIGHTNESS;
int lineBrightness  = DEFAULT_LINE_BRIGHTNESS;

// Neopixel 색상정보
int white[3]       = {255, 255, 255};
int red[3]         = {255, 0,   0  };
int yellow[3]      = {255, 255, 0  };
int green[3]       = {0,   255, 0  };
int purple[3]      = {255, 0,   255};

int line_white[3]  = {255, 255, 255};
int line_red[3]    = {255, 0,   0  };
int line_yellow[3] = {255, 255, 0  };
int line_green[3]  = {0,   255, 0  };
int line_purple[3] = {255, 0,   255};

//================================ Rfid ==================================
Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);

bool send_rfid_error;    // Rfid 이상이 생길 시 true
bool rfid_tag;

void RfidInit();
void RfidLoop();
void CardChecking(uint8_t rfidData[32]);

//================================ Mp3 ===================================
DFRobotDFPlayerMini myDFPlayer;
bool mp3_available = false;
bool mp3_cool;
bool mp3_open;

void Mp3Init();
void Mp3Check();
void Mp3PlayLargeFolder(uint8_t folder_number, uint16_t file_number);

//================================ Timer =================================
// 타이머 사용 선언
SimpleTimer cooltime_timer;
SimpleTimer duct_close_timer;
SimpleTimer rfid_timer;
SimpleTimer wifi_timer;
SimpleTimer tagger_blink_timer;

int cooltime_timer_id;
int duct_close_timer_id;
int rfid_timer_id;
int wifi_timer_id;
int tagger_blink_timer_id;

void TimerRun();
void CooltimeTimerFunc();
void RfidTagTimerFunc();
void WifiTimerFunc();

//================================ Switch =================================

//================================ EMLock =================================

void EmlockCheck();

#endif
