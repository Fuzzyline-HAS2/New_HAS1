#ifndef _HAS1_ITEMBOX
#define _HAS1_ITEMBOX

#include <ArduinoJson.h>          // StaticJsonDocument — myDoc 타입 선언에 필요
#include "neopixel_hal.h"
#include "motor_hal.h"
#include "rfid_hal.h"
#include "encoder_hal.h"
#include "vibration_hal.h"

// 코어 간 Queue (wifi.ino에서 정의, WifiInit()에서 xQueueCreate로 생성)
struct SendMsg { char deviceName[32]; char key[32]; char value[64]; };
extern QueueHandle_t sendQueue;          // Core1 → Core0  (게임 이벤트 → HTTP POST)
extern QueueHandle_t receiveQueue;       // Core0 → Core1  (서버 데이터 → DataChanged)
extern QueueHandle_t roleRequestQueue;   // Core1 → Core0  (tagUser → Receive 요청)
extern QueueHandle_t roleResponseQueue;  // Core0 → Core1  (role 결과 반환)
extern StaticJsonDocument<1000> myDoc;   // Core1 소유 — Core0이 직렬화해 전달, DataChanged()가 읽음

// Service Layer 공개 인터페이스 (wifi.ino)
void   WifiInit();
void   GameEventSend(const char* key, const char* value);
void   WifiRequestPlayer(const char* tagUser);  // role 조회 요청 — 논블로킹
String WifiPollPlayerRole();                    // role 조회 결과 폴링 — 논블로킹, 없으면 ""

// GAME SYSTEM==============================================================================
enum {VIBESTREGNTH = 0, ANSWER, RANGE};
enum {ANSWER_CNT = 0, ANSWER_RANGE, VIBRATION_RANGE};
int modeValue[3][5] = { {255, 190, 150, 110, 0},
                        {13,  43,  21,  0,   0},
                        {5,   2,   5,   0,   0}};  // RANGE[ANSWER_CNT]=5: 서버 puzzle_count 수신 전까지 최대값

// 서버에서 받은 퍼즐 정보 (Step 7에서 DataChanged()가 갱신)
unsigned long puzzleResetTime = 30000;   // PAUSED 상태 방치 시 ACTIVATE 복귀까지 대기 시간 (ms)

// 상태 머신 ================================================================================
enum GameState {
    GAME_SETTING,        // game_state=setting.  초기화, 박스 열림 (LED: WHITE)
    GAME_READY,          // game_state=ready.    외부 RFID 스캔, 태그 시 경고만 (LED: RED)
    GAME_ACTIVATE,       // game_state=activate. 외부 RFID 스캔, 태그 시 퍼즐 시작 (LED: YELLOW)
    GAME_PUZZLE,         // 퍼즐 진행 중, 외부 태그 유지 필요 (LED: BLUE)
    GAME_PAUSED,         // 태그 이탈 일시정지, 재태그 대기 (LED: YELLOW)
    GAME_CORRECT_ANIM,   // 정답 연출 (LED: GREEN 점멸) → 완료 후 GAME_PUZZLE or GAME_BOX_OPENING
    GAME_WRONG_ANIM,     // 오답 연출 (LED: RED 점멸) → 완료 후 GAME_PUZZLE
    GAME_BOX_OPENING,    // 모터 동작 중 (LED: GREEN) → MotorHalUpdate 완료 감지 → GAME_BOX_OPEN
    GAME_BOX_OPEN,       // 내부 RFID 아이템 태그 대기 (LED: NEO_INNER YELLOW)
    GAME_ITEM_FAIL_ANIM, // 배터리팩 초과 오류 연출 (LED: RED 점멸) → GAME_BOX_OPEN
    GAME_USED,           // 아이템 수령 완료, 모든 입력 차단 (LED: BLUE)
    GAME_DONE,           // 게임 종료 (repaired_all / win / lose), 모든 입력 차단 (LED: BLUE)
};
GameState gameState = GAME_DONE;  // setup()의 ChangeGameState(GAME_SETTING)으로 덮어씀

const char* GameStateName(GameState s) {
    switch (s) {
        case GAME_SETTING:        return "SETTING";
        case GAME_READY:          return "READY";
        case GAME_ACTIVATE:       return "ACTIVATE";
        case GAME_PUZZLE:         return "PUZZLE";
        case GAME_PAUSED:         return "PAUSED";
        case GAME_CORRECT_ANIM:   return "CORRECT_ANIM";
        case GAME_WRONG_ANIM:     return "WRONG_ANIM";
        case GAME_BOX_OPENING:    return "BOX_OPENING";
        case GAME_BOX_OPEN:       return "BOX_OPEN";
        case GAME_ITEM_FAIL_ANIM: return "ITEM_FAIL_ANIM";
        case GAME_USED:           return "USED";
        case GAME_DONE:           return "DONE";
    }
    return "?";
}

// HAL Runnable 스케줄러 ===================================================================
// "언제 실행할지"(TickRunnables)와 "무엇을 할지"(HAL 함수)를 분리한다.
// HAL 함수 내부에는 millis() 없음 — due 플래그가 true일 때만 호출됨을 보장.
struct Runnable {
    uint32_t periodMs;
    uint32_t lastRun;
    bool     due;
};

Runnable motorR   = {  10, 0, false };  // MotorHalUpdate   10ms
Runnable encoderR = {  20, 0, false };  // EncoderHalUpdate 20ms
Runnable blinkR   = {  50, 0, false };  // BlinkHalUpdate   50ms (Step 4에서 사용)
Runnable rfidR    = { 200, 0, false };  // RfidHalUpdate   200ms

// 게임 진행 상태 변수 =====================================================================
int  answerCnt        = 0;
bool lastButtonPressed = false;

unsigned long rfidLastSeenTime  = 0;
const unsigned long RFID_PUZZLE_TIMEOUT = 500;

unsigned long pauseStartTime = 0;

// ANIM 상태 공유 변수 (ChangeGameState에서 초기화, Step 4 BlinkHalUpdate에서 사용)
int  blinkCnt = 0;
bool ledOn    = false;

#endif
