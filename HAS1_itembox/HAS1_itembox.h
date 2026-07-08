#ifndef _HAS1_ITEMBOX
#define _HAS1_ITEMBOX

#include "library_and_pin.h"

// LOG======================================================================================
// 로그 형식: [MODULE] message  예: [GAME ] WAIT_TAG -> PUZZLE
// 모듈 태그: MAIN / MP3 / ENC / MOTOR / VIB / RFID / GAME / NEO
void Log(const char *tag, const String &msg) {
  char head[12];
  snprintf(head, sizeof(head), "[%-5s] ", tag);
  Serial.print(head);
  Serial.println(msg);
}

// DFPLAYER=================================================================================
SoftwareSerial MP3Serial(DFPLAYER_RX_PIN, DFPLAYER_TX_PIN);  // RX=39, TX=33
DFRobotDFPlayerMini myDFPlayer;
bool dfPlayerReady = false;  // 초기화 성공 여부 (실패 시 오디오 없이 동작)

// LINER MOTOR (BTS7960)====================================================================
#define MOTOR_FREQ       5000  // 5kHz (BTS7960 motor driver는 25kHz까지 허용)
#define MOTOR_RESOLUTION 8     // 듀티 0~255
#define MOTOR_SPEED      255   // 최대 출력 사용
#define BOX_OPEN_TIME    4000  // 박스 여는 시간 (ms)
enum BoxState { BOX_IDLE, BOX_OPENING, BOX_CLOSING };
BoxState boxState = BOX_IDLE;
unsigned long boxOpenStartTime = 0;
unsigned long switchHighSince = 0;              // 스위치 HIGH 시작 시각 (0 = 안 눌림)
const unsigned long SWITCH_DEBOUNCE_TIME = 50;  // 이 시간 연속 HIGH여야 눌림 인정 — 모터 노이즈 스파이크 필터 (ms)

// VIBRATION MOTOR==========================================================================
#define VIB_MOTOR_FREQ       5000  // 5kHz (TB6612FNG는 100kHz까지 허용)
#define VIB_MOTOR_RESOLUTION 8     // 듀티 0~255

// ENCODER==================================================================================
ESP32Encoder encoder;
#define ENCODER_MAX 95
#define ENCODER_MIN 0

// RFID=====================================================================================
Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS1);

// GAME SYSTEM===============================================================================
enum {VIBESTREGNTH = 0, ANSWER, RANGE};             // modeValue 행 인덱스
enum {ANSWER_CNT = 0, ANSWER_RANGE, VIBRATION_RANGE}; // RANGE 행의 열 인덱스
int modeValue[3][5] = { {255, 190, 150, 110, 0}, // VIBESTREGNTH (모터 세기): 0~255 설정 가능, 정답에 가까울수록 강함
                        {13,  43, 21,  0, 0},   // ANSWER (Puzzle 정답): 0~ENCODER_MAX 설정 가능
                        {3,    2,  5,  0, 0}};  // RANGE {정답개수, 정답 범위, 진동 단계 범위}

// 게임 상태 머신: 태그 대기 → 퍼즐 진행 ⇄ 일시정지(태그 이탈) → 해결(박스 열림)
// 상태 전환은 Game_system.ino의 각 상태 함수에서만 일어난다
enum GameState { GAME_WAIT_TAG, GAME_PUZZLE, GAME_PAUSED, GAME_SOLVED };
GameState gameState = GAME_WAIT_TAG;
const char* GameStateName(GameState s) {  // 상태 전환 로그용 이름표
  switch (s) {
    case GAME_WAIT_TAG: return "WAIT_TAG";
    case GAME_PUZZLE:   return "PUZZLE";
    case GAME_PAUSED:   return "PAUSED";
    case GAME_SOLVED:   return "SOLVED";
  }
  return "?";
}
int answerCnt = 0;               // 맞춘 정답 개수 (현재 몇 번째 문제인지)
bool lastButtonPressed = false;  // 엔코더 버튼 에지 검출용
unsigned long rfidLastSeenTime = 0;             // 퍼즐 중 RFID 마지막 감지 시각
const unsigned long RFID_PUZZLE_TIMEOUT = 500; // 태그 이탈 판정 시간 (ms)
unsigned long pauseStartTime = 0;               // 일시정지 진입 시각
const unsigned long PUZZLE_RESET_TIME = 30000;  // 일시정지 상태로 30초 지나면 퍼즐 리셋 (ms)
unsigned long lastRfidCheckTime = 0;            // 마지막 태그 확인 시각
const unsigned long RFID_CHECK_INTERVAL = 200;  // 태그 확인 주기 (ms) — PN532 통신이 느려서 매 loop 확인하면 렉 발생
bool waitTagRelease = false;                    // true면 카드를 뗐다가 다시 태그해야 다음 동작 인정 (연속 인식 방지)
unsigned long tagAbsentSince = 0;               // 카드 미감지 시작 시각
const unsigned long TAG_RELEASE_TIME = 700;     // 이 시간 동안 미감지면 "카드 뗌"으로 판정 (ms)

// NEOPIXEL======================================================================================
#define LED_BRIGHTNESS 127                    // 고정 밝기 50% (0~255 스케일)
const int NeopixelNum = 2;                    // 설치된 네오픽셀 스트립 개수
enum {NEO_PN532 = 0, NEO_ENCODER};            // pixels[] 인덱스 (RFID/엔코더 객체와 구분을 위해 NEO_ 접두사)
const int NumPixels[NeopixelNum] = {28, 24};  // 스트립별 픽셀 개수
enum {WHITE = 0, RED, YELLOW, GREEN, BLUE, PURPLE, BLACK, BLUE0, BLUE1, BLUE2, BLUE3};
// Neopixel 색상정보
int color[11][3] = {{255, 255, 255}, //WHITE
                    {255, 0,   0  }, //RED
                    {255, 255, 0  }, //YELLOW
                    {0,   255, 0  }, //GREEN
                    {0,   0,   255}, //BLUE
                    {255, 0,   255}, //PURPLE
                    {0,   0,   0  }, //BLACK
                    {0,   0,   64 }, //ENCODERBLUE0
                    {0,   0,   128}, //ENCODERBLUE1
                    {0,   0,   192}, //ENCODERBLUE2
                    {0,   0,   255}}; //ENCODERBLUE3

Adafruit_NeoPixel pixels[NeopixelNum] = {Adafruit_NeoPixel(NumPixels[NEO_PN532], PN532_NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800),
                                         Adafruit_NeoPixel(NumPixels[NEO_ENCODER], ENCODER_NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800)};

#endif
