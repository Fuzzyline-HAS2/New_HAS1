// =================================================================================
// HAS1_generator.h
// ---------------------------------------------------------------------------------
// "발전기(Generator)" 기기의 전역 상태·객체·함수 프로토타입을 모아놓은 헤더.
// 이 기기는 4개의 배선을 꽂아 배터리팩을 충전한 뒤, 손잡이(엔코더)를 돌려
// 게이지를 채우는 "스타터" 미니게임을 완료하면 수리(repaired) 상태가 되는 구조다.
//
// Arduino IDE는 같은 스케치 폴더의 모든 .ino 탭을 하나의 번역 단위로 합쳐서 컴파일하므로,
// 이 헤더에서 선언한 전역 변수/함수는 Game_system.ino, dfplayer.ino, neopixel.ino,
// rfid.ino, timer.ino, wifi.ino, wire.ino, encoder.ino 전체에서 공유해서 쓸 수 있다.
// =================================================================================
#ifndef _DONE_ITEMBOX_CODE_
#define _DONE_ITEMBOX_CODE_

#include "library_and_pin.h"
const int rfid_num = 1; // 설치된 pn532의 개수

//****************************************WIFI****************************************************************
// has2wifi: 서버(172.30.1.5:8080)와 주기적으로 통신하며 my/tag 등 JSON 상태를 동기화하는 객체.
//           TimerRun() -> WifiIntervalFunc() -> has2wifi.Loop(DataChanged) 로 매 tick 폴링된다.
HAS2_Wifi has2wifi("http://172.30.1.5:8080");

// ota: GitHub Releases(HAS1_generator 태그)에 올라간 update.bin을 내려받아 검증/적용하는 OTA 객체.
//      HMAC_SECRET으로 서명을 검증하고, FIRMWARE_VER보다 서버 버전이 높을 때만 업데이트를 진행한다.
SecureOTA ota(
    "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_generator/update.bin",
    "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_generator/version.txt",
    "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_generator/update.sig",
    HMAC_SECRET,
    FIRMWARE_VER
);
void DataChanged();     // 서버 데이터(my)가 바뀔 때마다 호출되어 게임 상태 전환을 처리 (wifi.ino)
void SettingFunc(void); // game_state == "setting" 진입 시 처리 (wifi.ino)
void ActivateFunc(void);// game_state == "activate" 진입 시 처리 (wifi.ino)
void ReadyFunc(void);   // game_state == "ready" 진입 시 처리 (wifi.ino)

bool receiveMineOn = false; // true인 동안은 DataChanged()가 서버 폴링 결과로 상태를 다시 덮어쓰지 않도록 막는 플래그
//****************************************Game System****************************************************************
void (*ptrCurrentMode)();   //현재모드 저장용 포인터 함수 — loop()에서 매 프레임 호출됨 (예: WaitFunc, RfidLoopMain, WirePollMain, StarterActivate)
void (*ptrRfidMode)();      //rfid모드 저장용 포인터 함수 — RFID 태그 인식 성공 시 호출됨 (예: WaitFunc, StartFinish)

void WaitFunc();        // 아무 것도 하지 않는 대기 상태용 콜백 (Game_system.ino 쪽엔 정의 없고 wifi.ino에 빈 함수로 정의됨)
void StarterActivate(); // 엔코더를 돌려 게이지를 채우는 "스타터" 미니게임 루프 (Game_system.ino)
const unsigned long starterNeoDivider = 15000*7.5; // (현재 코드에서 직접 사용되지 않는 상수로 보임 — 과거 로직의 잔재)
int starterEncoderUnit = 4000;        // 게이지 1칸당 필요한 엔코더 값
int starterDecreaseAmount = 1125;     // 2초마다 감소하는 엔코더 양
bool blinkOn = false; // BlinkTimerFunc()가 네오픽셀을 켬/끔 번갈아 할 때 현재 상태를 기억하는 플래그
//****************************************Timer System****************************************************************
// SimpleTimer 인스턴스 3개 — 각각 독립된 인터벌로 TimerRun()에서 매 loop마다 run()된다.
SimpleTimer GameTimer;  // 스타터 진행 중 엔코더 값 감소를 주기적으로 트리거
SimpleTimer WifiTimer;  // 서버 폴링(has2wifi.Loop) 주기 트리거

SimpleTimer BlinkTimer; // 특정 네오픽셀을 깜빡이게 하는 주기 트리거

void TimerInit();        // 세 타이머의 인터벌을 등록(및 즉시 정지)함 — setup()에서 1회 호출 (timer.ino)
void WifiIntervalFunc(); // WifiTimer 콜백: 서버와 통신 (timer.ino)
void GameTimerFunc();    // GameTimer 콜백: 엔코더 값을 서서히 감소시켜 방치 시 게이지가 줄어들게 함 (timer.ino)


void BlinkTimerFunc();                    // BlinkTimer 콜백: blinkNeo/blinkColor로 지정된 네오픽셀을 토글 (timer.ino)
void BlinkTimerStart(int Neo, int NeoColor); // 특정 네오픽셀(Neo)을 특정 색(NeoColor)으로 깜빡이기 시작 (timer.ino)
int blinkNeo = 0;   // 현재 깜빡이고 있는 네오픽셀 인덱스 (GAUGE/STARTER/DEVICESTATE/CIRCUIT)
int blinkColor = 0; // 현재 깜빡임에 사용 중인 색상 인덱스 (color[] 배열의 인덱스)

int wifiTimerId; // WifiTimer에 등록된 인터벌의 ID (deleteTimer 등에 사용)
int gameTimerId; // GameTimer에 등록된 인터벌의 ID

int blinkTimerId; // BlinkTimer에 등록된 인터벌의 ID

unsigned long wifiTime = 2000;    // 1sec  (실제 값은 2000ms — 주석은 과거 값의 잔재로 보임)

unsigned long gameTime = 400;    // 3sec  (실제 값은 400ms — 위와 동일)
unsigned long blinkTime = 1800;   // 1sec  (실제 값은 1800ms — 위와 동일)


volatile unsigned int gameTimerCnt; // GameTimerFunc 호출 횟수를 세는 카운터 — 5회(=2초)마다 엔코더 값을 감소시킴
//****************************************DFPLAYER SETUP****************************************************************
// TODO: Nextion → DFPlayer 교체 중. 아래 음원 재생 호출은 전부 임시로 (폴더1, 트랙1)을 재생함 —
// 실제 폴더/트랙 번호는 각 호출부(wifi.ino, rfid.ino, dfplayer.ino)에서 직접 지정할 것.
void Mp3_Init();  // DFPlayer 초기화 (dfplayer.ino)
void Mp3PlayLargeFolder(uint8_t folder_number, uint16_t file_number); // 지정 폴더/트랙 재생 (dfplayer.ino)
void LeftGenerator();   // 남은 발전기 개수 안내 음원 재생 (dfplayer.ino)
void BatteryPackSend(); // 배선 충전량을 게이지 네오픽셀에 반영 + 음원 재생 (dfplayer.ino)
//****************************************Neopixel SETUP****************************************************************
#define DEFAULT_BRIGHTNESS 50
int ledBrightness = DEFAULT_BRIGHTNESS; // 현재 적용 중인 네오픽셀 밝기 (0~255, 서버 값으로 UpdateBrightness()에서 갱신됨)
void NeopixelInit();       // 4개 네오픽셀 스트립 초기화 + 전체 흰색 점등 (neopixel.ino)
void UpdateBrightness();   // 서버에서 받은 밝기(1~100)를 0~255로 매핑해 전체 스트립에 적용 (neopixel.ino)
void EncoderNeopixelOn();  // 스타터 게이지 진행률만큼 GAUGE 네오픽셀을 채움 (neopixel.ino)
void NeoBlink(int neo, int neoColor, int cnt, int blinkTime); // 지정 스트립을 cnt회 blocking 방식으로 점멸 (neopixel.ino)
void BatteryGaugeShow(int cnt, int maxCnt); // 배선 충전 비율(cnt/maxCnt)만큼 GAUGE 네오픽셀을 채움 (neopixel.ino)
const int NumPixels[4] = {28,4,16,10}; // 각 스트립(GAUGE/STARTER/DEVICESTATE/CIRCUIT)의 픽셀 개수
const int NeopixelNum = 4;             // 네오픽셀 스트립(용도) 개수
enum {GAUGE = 0, STARTER, DEVICESTATE, CIRCUIT};                          // pixels[] / NumPixels[] 인덱스
enum {WHITE = 0, RED, YELLOW, GREEN, BLUE, PURPLE, BLACK, BLUE0, BLUE1, BLUE2, BLUE3}; // color[] 인덱스
// Neopixel 색상정보
int color[11][3] = {    {255, 255, 255}, //WHITE
                        {255, 0,   0},   //RED
                        {255, 255, 0},   //YELLOW
                        {0,   255, 0},   //GREEN
                        {0,   0,   255}, //BLUE
                        {255, 0,   255}, //PURPLE
                        {0,   0,   0},   //BLACK
                        {0,   0,   64},  //ENCODERBLUE0
                        {0,   0,   128}, //ENCODERBLUE1
                        {0,   0,   192}, //ENCODERBLUE2
                        {0,   0,   255}}; //ENCODERBLUE3

const int neopixel_num = 4; // 설치된 네오픽셀의 개수 (NeopixelNum과 동일한 값의 별도 상수)

// 실제 4개의 Adafruit_NeoPixel 객체 — 위 GAUGE/STARTER/DEVICESTATE/CIRCUIT 순서와
// library_and_pin.h에 정의된 4개의 데이터 핀이 1:1로 대응된다.
Adafruit_NeoPixel pixels[NeopixelNum] = {Adafruit_NeoPixel(NumPixels[GAUGE], PN532_NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800),
                                         Adafruit_NeoPixel(NumPixels[STARTER], ENCODER_NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800),
                                         Adafruit_NeoPixel(NumPixels[DEVICESTATE], INNER_NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800),
                                         Adafruit_NeoPixel(NumPixels[CIRCUIT], CIRCUIT_NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800)
                                         };

//****************************************RFID SETUP****************************************************************
enum {MAINPN532 = 0}; // nfc[] 배열 인덱스 — 현재 리더가 1개뿐이라 MAINPN532만 존재

Adafruit_PN532 nfc[rfid_num] = {Adafruit_PN532(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS1)};

bool rfid_tag;          // (현재 코드에서 직접 참조되지 않는 전역 — 과거 로직의 잔재로 보임)
bool rfid_timer_assess; // (현재 코드에서 직접 참조되지 않는 전역 — 과거 로직의 잔재로 보임)

bool rfid_init_complete[rfid_num]; // 각 리더의 초기화 성공 여부 (RfidInit에서 설정)
void RfidInit(void);   // PN532 리더 초기화 (rfid.ino)
// 근접 인식 Dead Zone 대응용 태그 유무 확인 헬퍼 (rfid.ino 구현, 근/원거리 Gain 자동 재시도).
// StarterActivate(Game_system.ino)에서도 사용하므로 여기서 프로토타입 선언.
bool RfidPresenceCheck();
void RfidLoop(void);   // (프로토타입만 존재 — 실제 정의는 RfidLoopMain()이라는 이름으로 rfid.ino에 있음)
void CheckingPlayers(uint8_t user, uint8_t user_num, uint8_t rfid_num); // (프로토타입만 존재 — 실제 정의는 rfid.ino의 CheckingPlayers(uint8_t rfidData[32])와 시그니처가 다름, 미사용 프로토타입으로 보임)


//****************************************WIRE SETUP****************************************************************
// 배선 4개 감지로 배터리팩 충전 — 기존 RFID 태그 방식(BatteryPackCharge) 대체
void WireInit();          // 배선 감지 핀 4개를 INPUT_PULLUP으로 설정 (wire.ino)
int  WireCountPlugged();  // 현재 꽂혀 있는(LOW인) 배선 개수를 반환 (wire.ino)
void WireResetTracking(); // 디바운스 추적 상태 초기화 — 충전 단계 진입 시 호출 (wire.ino)
void WirePollMain();      // loop()에서 매 프레임 호출되어 배선 개수 변화를 디바운스 후 반영 (wire.ino)

//****************************************ENCODER SETUP****************************************************************
// 인터럽트(ISR) 대신 ESP32 하드웨어 펄스 카운터(PCNT) 사용
void EncoderInit();     // PCNT 유닛/채널 설정 및 카운팅 시작 — setup()에서 1회 호출 (encoder.ino)
void EncoderAttach();   // 카운팅 시작 (재시작) — 값은 초기화하고 이어서 셈
void EncoderDetach();   // 카운팅 정지 — 스타터 단계가 아닐 때 불필요한 카운팅을 막음
void EncoderLoop();     // 매 loop마다 하드웨어 카운터 → encoderValue 반영
bool encoderAttached = false; // EncoderInit()에서 true로 전환 — 현재 PCNT가 카운팅 중인지 여부

long encoderValue = 0;     //현재 엔코더 값 — starterEncoderUnit으로 나눈 값이 스타터 게이지 칸 수가 됨

#endif
