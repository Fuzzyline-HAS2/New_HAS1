#ifndef _HAS1_TAGMACHINE_SUB_
#define _HAS1_TAGMACHINE_SUB_

#include "library_and_pin.h"
#include "recovery_policy.h"
#include <TagMachineOtaProtocol.h>
const int rfid_num = 3; // 설치된 pn532의 개수

extern HardwareSerial fromSubSerial;
//****************************************SimpleTimer SETUP****************************************************************
extern SimpleTimer GameTimer;
void TimerInit();
void GameTimerFunc();
extern int gameTimerId;

//****************************************Pointer System****************************************************************
extern void (*ptrCurrentMode)();   //현재모드 저장용 포인터 함수

//****************************************RFID SETUP****************************************************************
extern Adafruit_PN532 nfc;
bool RfidInit(void);
void RfidLoopMain(void);
void RequestRfidReinit(bool recoveryAckRequired);
extern bool rfid_init_complete;

//****************************************Signed OTA SETUP****************************************************************
extern const uint32_t tagmachineFirmwareVersion;
extern const uint32_t tagmachinePartitionVersion;
extern uint32_t tagmachineBootId;
void OtaInit();
void OtaPoll();
void OtaReplayResult();
void OtaRequest(uint32_t requestId, uint32_t targetVersion);
void OtaHandleCommand(const tagmachine::ota_wire::Command &command);
bool OtaBusy();
bool OtaHealthy(uint32_t now);

// PN532가 없을 때도 한 번의 폴링이 유한 시간 안에 끝나도록 한다.
#define RFID_ACTIVATION_RETRIES 10
#define RFID_DETECT_TIMEOUT_MS 250
#define RFID_TAG_DATA_LENGTH 4

// 정상 PN532 명령은 최악에도 5~8초 안에 반환한다. 12초 WDT는 소프트웨어 복구마저
// 실행할 수 없는 진짜 교착만 재부팅하며, 평상시 카드 탐색에는 개입하지 않는다.
#define BEETLE_WATCHDOG_TIMEOUT_MS 12000
#define BEETLE_HELLO_INTERVAL_MS 1000
#define BEETLE_HEARTBEAT_INTERVAL_MS 2000
#define BEETLE_OTA_TIMEOUT_MS 180000
#define RFID_POLL_INTERVAL_MS 50

// 근접 인식 Dead Zone 대응 — RxGain 동적 전환 (rfid.ino 구현).
// GainMode는 반드시 여기(헤더)서 정의해야 한다 — Arduino가 .ino 탭들을 병합할 때 자동
// 생성하는 함수 프로토타입을 스케치 맨 앞(이 헤더 include 다음, 각 .ino의 실제 코드보다 앞)에
// 삽입하므로, rfid.ino 안에서만 정의하면 그 프로토타입 자리에서 "GainMode를 아직 모른다"는
// 컴파일 에러가 난다.
enum GainMode { GAIN_NEAR, GAIN_FAR };

// 유지 중이던 태그가 두 Gain 모두에서 이 시간 이상 연속으로 안 잡히면 그제서야 제거로 판정.
// (단 한 번의 Read 실패로 바로 태그 제거 처리하지 않기 위한 디바운스 — 500~1000ms 범위에서 조정 가능)
#define TAG_REMOVE_TIME_MS 500
#endif
