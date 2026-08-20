#ifndef _HAS1_TAGMACHINE_MAIN_
#define _HAS1_TAGMACHINE_MAIN_

#include "Library_and_pin.h"

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

const int rfid_num = 1; // 설치된 pn532의 개수

//****************************************WIFI****************************************************************
void TelnetInit();
void TelnetRun();
HAS2_Wifi has2wifi("http://172.30.1.43");
SecureOTA ota(
  "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_tagmachine_main/update.bin",
  "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_tagmachine_main/version.txt",
  "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_tagmachine_main/update.sig",
  HMAC_SECRET,
  FIRMWARE_VER
);

// Main/Sub Beetle(ESP32-C3)은 자체 WiFi가 없어 GitHub에서 직접 OTA를 받을 수 없으므로,
// TTGO가 대신 이 URL들에서 다운로드 + HMAC 서명 검증까지 마친 뒤 UART로 릴레이해준다
// (beetle_ota.ino). Beetle 쪽 배포는 devices/HAS1_tagmachine_sub/scripts/deploy.py가 담당.
const char* BEETLE_FIRMWARE_URL = "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_tagmachine_sub/update.bin";
const char* BEETLE_VERSION_URL  = "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_tagmachine_sub/version.txt";
const char* BEETLE_SIG_URL      = "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_tagmachine_sub/update.sig";
void CheckAndUpdateBeetles();
void DataChanged();
void ApplyDeviceState(String deviceState);
void QueuePendingDeviceState(String deviceState);
void ApplyPendingDeviceState();
void SettingFunc(void);
void ActivateFunc(void);
void ReadyFunc(void);
void GameSetting();
void WaitFunc();
void WifiIntervalLoop(unsigned long intervalValue);
unsigned long wifiInterval = 0;
int wifiNum = 0;
struct WIFISSID {
  String strDevice;
};
struct WIFISSID wifissid[5] = {{"badland_auto"},
                               {"badland_ruins"},
                               {"badland_shoot"},
                               {"badland_prison"},
                               {"badland_chack"}};
//****************************************Pointer
//System****************************************************************
void (*ptrCurrentMode)() = nullptr; // 현재모드 저장용 포인터 함수 (setup 이전
                                    // nullptr → loop에서 null 가드)
void (*ptrRfidMode)(char inputRole) = nullptr; // rfid모드 저장용 포인터 함수
void (*ptrRfidFail)() = nullptr;  // rfidFail모드 저장용 포인터 함수
void (*ptrRfidMain)() = nullptr;  // rfid 메인 저장용 포인터 함수
void (*ptrRfidSub)() = nullptr;   // rfid 서브 저장용 포인터 함수
void (*ptrGameTimer)() = nullptr; // 게임 타이머에 들어가는 포인터 함수
void NewbieTaggerUnlockTimerFunc();
//****************************************Game
//System****************************************************************
volatile int playerLockTime = 7;
volatile int playerUnlockTime = 10;
volatile int taggerUnlockTime = 15;
volatile int ghostOpenTime = 15;
void WhichTagged();
void DoorOpen();
void NewbiePlayerOpenFunc();
void NewbieGhostOpenFunc();
void NewbieLogin(char role);
void NewbiePlayerOpen();
void NewbieGhostOpen();
void NewbieModeSetting();

String strCurState = "";
String pendingDeviceState = "";
bool pendingDeviceStateApply = false;
//****************************************Serial
//Communication*********************************************************
void CommnunicationBeetle();
void CommnunicationMainBeetle();
void SubSerialFlush();
void MainSerialFlush();
void SendBeetleTag(int idx, const char *field, const String &tagUser);
enum { BEETLE_SUB = 0, BEETLE_MAIN };
unsigned long beetleSendLastMs[2] = {0, 0}; // 채널별 서버 전송 스로틀용 (1초)
HardwareSerial toSubSerial(1);
HardwareSerial toMainSerial(2);
bool tagState = false;
//****************************************SimpleTimer
//SETUP****************************************************************
SimpleTimer GameTimer;
SimpleTimer WifiTimer;
SimpleTimer SubSerialTimer;
SimpleTimer DebuffTimer;

void TimerInit();
void WifiIntervalFunc();
void GameTimerFunc();
void SubSerialTimerFunc();
void DebuffTimerFunc();

int wifiTimerId;
int gameTimerId;
int subSerialTimerId;
int debuffTimerId;

int gameTimerCnt = 0;
bool SubSerialTimerStart = false;
// [DFPlayer 비활성화] DFPlayer 원인 분리 테스트용 주석처리
// //****************************************DFPlayer SETUP
// SoftwareSerial MP3Serial(DFPLAYER_RX_PIN, DFPLAYER_TX_PIN);
// DFRobotDFPlayerMini myDFPlayer;
// void Mp3_Setup();
// enum { VD1 = 1, VD2, VD3, VD4, VD5, VD6, VD7, VD8, VD9, VD10, VD11 };
// bool send_mp3_err = false;
enum { VD1 = 1, VD2, VD3, VD4, VD5, VD6, VD7, VD8, VD9, VD10, VD11 };
bool send_mp3_err = false;
//****************************************Neopixel
//SETUP****************************************************************
void NeopixelInit();
void NeoBlink(int neo, int neoColor, int cnt, int blinkTime);
const int NumPixels[3] = {16, 60, 10};
const int NeopixelNum = 5;
enum { LINE = 0, ROUND, ONBOARD, LINE_SUB, ROUND_SUB };
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

#define DEFAULT_BRIGHTNESS 50
int ledBrightness = DEFAULT_BRIGHTNESS;
void UpdateBrightness();

const int neopixel_num = 5; // 설치된 네오픽셀의 개수

Adafruit_NeoPixel pixels[NeopixelNum] = {
    Adafruit_NeoPixel(NumPixels[LINE], MAIN_LINE_NEOPIXEL_PIN,
                      NEO_GRB + NEO_KHZ800),
    Adafruit_NeoPixel(NumPixels[ROUND], MAIN_ROUND_NEOPIXEL_PIN,
                      NEO_GRB + NEO_KHZ800),
    Adafruit_NeoPixel(NumPixels[ONBOARD], ONBOARD_NEOPIXEL_PIN,
                      NEO_GRB + NEO_KHZ800),
    Adafruit_NeoPixel(NumPixels[LINE], SUB_LINE_NEOPIXEL_PIN,
                      NEO_GRB + NEO_KHZ800),
    Adafruit_NeoPixel(NumPixels[ROUND], SUB_ROUND_NEOPIXEL_PIN,
                      NEO_GRB + NEO_KHZ800)};

//****************************************RFID
//SETUP****************************************************************
bool mainRfidTagged = false;

void CheckingPlayers(String tagUser);

void CancelTagProgress();
void Login(char role);
void LoginTimerSelector(char role);
void LockFail();
void UnlockFail();
void WaitRfid(char role);

bool loginDone = false; // 처음 로그인 하면 true로 바뀌어서 wifi 안쓰게하기위함
char loginRole = 'P'; // 처음 로그인 하고 나면 role 저장용.
#endif
