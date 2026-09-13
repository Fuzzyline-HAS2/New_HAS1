#ifndef _HAS1_ESCAPE_MAIN_H_
#define _HAS1_ESCAPE_MAIN_H_

#include "library_and_pin.h"
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
// DataChanged()가 "이미 반영한 상태"로 기억하는 스냅샷. 예전에는 DataChanged 안의
// 함수 static이었는데, MMMM 핸들러가 로컬로 상태를 바꿔도 여기에 반영할 수 없어서
// 다음 서버 폴링 때 DataChanged가 그 전환을 처음 보는 변경으로 오인했다.
StaticJsonDocument<2048> cur;
void HandleMmmmCard();
void HandleTagPacket(String command);
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

// MMMM 관리자 카드 재무장 제어.
// Beetle의 RfidLoopMain()은 카드가 리더에 얹혀 있는 동안 매 루프마다 'M'을 보낸다.
// 카드를 잠깐만 대도 'M'이 수십 개 나가고, ActivateFunc/ReadyFunc가 모터 때문에
// 4~6초 블로킹하는 사이 그것들이 UART 버퍼에 쌓였다가 한꺼번에 처리되어
// ready <-> activate 가 반복 토글됐다(현장 2026-09-13).
//
// 마지막으로 'M'을 본 시각을 계속 갱신하고, 그 뒤 MMMM_REARM_MS 동안 조용해야
// 다음 태그로 인정한다. 즉 카드를 떼야 다시 동작한다.
unsigned long lastMmmmSeenMs = 0;
const unsigned long MMMM_REARM_MS = 1500;
void DrainSubSerial();
String lastBeetleRawPacket = ""; // LOGIC_SERIAL_02: 마지막 수신 T 패킷 원문
int invalidCmdCount = 0;         // LOGIC_SERIAL_03: 허용되지 않은 명령 수신 횟수
unsigned long resyncFragmentCount = 0;  // 잘린 줄(전송 도중 끊긴 조각) 폐기 횟수. 복구 로직에 넣지 않는다.
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
bool SendStateWithRetry(const String& column, const String& value, uint8_t retries = 3);
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
// Beetle 읽기 전용 타이머. 예전에는 GameTimer(500ms)와 WifiTimer(2000ms)에 얹어
// 읽었는데, 그러면 태그가 TTGO에 도달하기까지 최대 그 주기만큼 기다린다.
// 실측상 이게 태그 반응속도의 실제 병목이었다(Beetle 전송 속도가 아니라).
SimpleTimer BeetleTimer;
void TimerInit();
void WifiIntervalFunc();
void GameTimerFunc();
int wifiTimerId;
int gameTimerId;
int beetleTimerId;

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
