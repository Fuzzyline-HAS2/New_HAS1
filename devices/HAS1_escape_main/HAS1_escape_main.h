#ifndef _HAS1_ESCAPE_MAIN_H_
#define _HAS1_ESCAPE_MAIN_H_

#include "library_and_pin.h"
#include "QC/QC_Engine.h"
#include "QC/QC_Rules.h"
#include <WiFi.h>

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

//****************************************WIFI****************************************************************
HAS2_Wifi has2wifi("http://172.30.1.43");
SecureOTA ota(
    "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_escape_main/update.bin",
    "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_escape_main/version.txt",
    "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_escape_main/update.sig",
    HMAC_SECRET,
    FIRMWARE_VER
);
void DataChanged();
void SettingFunc(void);
void ActivateFunc(void);
void ReadyFunc(void);
void WaitFunc();
void WifiIntervalLoop(unsigned long intervalValue);
unsigned long wifiInterval = 0;
//****************************************Pointer
//System****************************************************************
void (*ptrCurrentMode)(); // 현재모드 저장용 포인터 함수

//****************************************Serial
//Communication*********************************************************
void CommnunicationBeetle();
bool PlayerDetector(String playerNum);
HardwareSerial toSubSerial(1); //
String tag1;
String tag2;
String tag3;
bool tagState[3] = {false, false, false};
// tagged_players 서버 전송은 오디오 재생과 무관하므로, 태그 감지 직후 즉시 보내지
// 않고 TagCount()(오디오 재생) 이후로 미뤄서 오디오까지의 텀에서 HTTP 왕복 1회를 뺀다.
bool tagValuePending = false;
String pendingTagValue = "";
void FlushPendingTagSend();
//****************************************Game
//System****************************************************************
int tagCnt = 0;
unsigned long lastBeetleMs = 0; // Beetle 마지막 수신 시각
String lastBeetleRawPacket = ""; // LOGIC_SERIAL_02: 마지막 수신 T 패킷 원문
int invalidCmdCount = 0;         // LOGIC_SERIAL_03: 허용되지 않은 명령 수신 횟수
int packetFormatErrorCount = 0;  // LOGIC_SERIAL_02: T 패킷 포맷 오류 누적
int tagParseErrorCount = 0;      // LOGIC_TAG_02: 태그 파싱 실패 누적
uint8_t beetleBadEventStreak = 0;   // 연속 bad-event 사이클 수 (silence 제외)
uint8_t beetleRecoverAttempts = 0;  // Beetle UART 복구 시도 횟수
//****************************************Recovery
//Functions****************************************************************
void HandleRuntimeRecovery();
void RecoverBeetleConnection();
void ResetBeetleErrorCounters();
bool SendDeviceStateWithRetry(const String& value, uint8_t retries = 3);
bool ClearGithubOtaState();
//****************************************Step
//Motor****************************************************************
void StepMotorInit();
void EscapeClose();
void EscapeOpen();
const int stepsPerRevolution = 100; // 기본세팅 200 AE탈장만 100으로 설정함
//****************************************SimpleTimer
//SETUP****************************************************************
SimpleTimer GameTimer;
SimpleTimer WifiTimer;
void TimerInit();
void WifiIntervalFunc();
void GameTimerFunc();
int wifiTimerId;
int gameTimerId;

//****************************************DFPlayer
//SETUP****************************************************************
HardwareSerial MP3Serial(2);
DFRobotDFPlayerMini myDFPlayer;
void Mp3_Setup();
enum { VE1 = 1, VE2, VE3, VE4, VE5 };
//****************************************Neopixel
//SETUP****************************************************************
void NeopixelInit();
void NeoBlink(int neo, int neoColor, int cnt, int blinkTime);
void AllNeoBlink(int neoColor, int cnt, int blinkTime);
void AllNeoOn(int neoColor);
void UpdateBrightness();
#define DEFAULT_BRIGHTNESS 50
int ledBrightness = DEFAULT_BRIGHTNESS;
enum { LINE = 0, ROUND, ONBOARD };
enum {
  WHITE = 0,
  RED,
  YELLOW,
  GREEN,
  BLUE,
  PURPLE,
  BLACK,
  BLUE0,
  BLUE1,
  BLUE2,
  BLUE3
};
int currentNeoColor = WHITE;
const int NumPixels[3] = {16, 60, 10};
const int NeopixelNum = 3;
// Neopixel 색상정보
int color[11][3] = {{255, 255, 255}, // WHITE
                    {255, 0,   0  }, // RED
                    {255, 255, 0  }, // YELLOW
                    {0,   255, 0  }, // GREEN
                    {0,   0,   255}, // BLUE
                    {255, 0,   255}, // PURPLE
                    {0,   0,   0  }, // BLACK
                    {0,   0,   64 }, // ENCODERBLUE0
                    {0,   0,   128}, // ENCODERBLUE1
                    {0,   0,   192}, // ENCODERBLUE2
                    {0,   0,   255}}; // ENCODERBLUE3

const int neopixel_num = 3; // 설치된 네오픽셀의 개수

Adafruit_NeoPixel pixels[NeopixelNum] = {
    Adafruit_NeoPixel(NumPixels[LINE], LINE_NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800),
    Adafruit_NeoPixel(NumPixels[ROUND], ROUND_NEOPIXEL_PIN,
                      NEO_GRB + NEO_KHZ800),
    Adafruit_NeoPixel(NumPixels[ONBOARD], ONBOARD_NEOPIXEL_PIN,
                      NEO_GRB + NEO_KHZ800)};

#endif
