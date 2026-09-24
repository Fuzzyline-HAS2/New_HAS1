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

// RFID 재판독 디바운스. 1000ms에서는 초당 1회만 폴링해 1초보다 짧은 태그를 놓쳤다
// (현장: "태그 인식 느림"). itembox와 동일한 300ms로 맞춤 (커밋 8dc9450 참고).
#define RFID_DEBOUNCE_MS 300
// 카드가 없을 때 InListPassiveTarget이 스스로 끝나기까지의 활성화 재시도 횟수(RFConfiguration
// item 5, MxRtyPassiveActivation). 기본 0xFF는 카드가 올 때까지 무한 대기라 다음 명령을 막는다.
// 한 번에 수 ms라 10회면 카드 없는 폴링 1회가 수십 ms 안에 끝난다.
#define RFID_ACTIVATION_RETRIES 10
// readPassiveTargetID()가 응답을 기다리는 상한(ms). 위 재시도가 끝나는 시간보다 넉넉하면 되고,
// PN532가 멈췄을 때 루프가 1000ms(라이브러리 기본)씩 묶이지 않게 하는 안전장치다.
#define RFID_DETECT_TIMEOUT_MS 250
bool rfid_tag = false;
byte rfid_tag_count = 0; // 몇번 태그 됐는지 (= 덕트를 몇 번 사용했는지) 확인하는 변수

// Situation() 전송 시점의 tagUser를 기억해뒀다가, 서버 승인으로 device_state="open"이
// 확정되는 시점(game_state.ino DataChange)에 그 iotGlove의 is_open을 true로 기록하는 데 쓴다.
String last_open_tag_user = "";

// ── 유효한 유령 태그 -> 실제 릴레이 HIGH 소요시간 실측 ──
// 사용자 Receive 이전부터 재고, SolenoidPulse가 반환하는 HIGH 시각으로 끝낸다.
// 결과는 USB Serial/Telnet 콘솔에만 출력하며 별도 네트워크 전송은 하지 않는다.
bool          ghost_open_pending    = false;
unsigned long ghost_tag_start_ms    = 0;
int           ghost_poll_count      = 0;
int           ghost_rssi_at_tag     = 0;
unsigned long ghost_situation_ms    = 0;
unsigned long ghost_role_receive_ms = 0;
#define GHOST_OPEN_TIMEOUT_MS 15000  // 로컬 타이밍 로그와 승인 대기의 공통 상한

// 진단용 ghost_open_pending과 별개로 모든 역할의 승인 요청을 한 번에 하나만 처리한다.
#define REVIVAL_APPROVAL_TIMEOUT_MS GHOST_OPEN_TIMEOUT_MS
#define REVIVAL_APPROVAL_POLL_MS 300
#define REVIVAL_ADMIN_POLL_MS 1000
// 600ms는 밀착 유지 중 PN532가 몇 초씩 간헐적으로 못 읽는 현상(현장 리포트)에는 너무 짧아서,
// 태그가 실제로는 계속 붙어있는데도 "빠졌다"고 오판해 매번 새 사이클(Situation 재전송 포함)을
// 돌렸다. 5초로 넉넉히 올려서 일시적 RF 미검출을 진짜 제거와 헷갈리지 않게 한다.
#define RFID_REARM_ABSENT_MS 5000
bool revival_approval_pending = false;
bool revival_approval_poll_due = false;
bool revival_approval_polled_this_loop = false;
unsigned long revival_approval_started_ms = 0;
unsigned long revival_approval_last_poll_ms = 0;
unsigned long revival_approval_last_admin_poll_ms = 0;
String revival_request_device_state = "";

// 계속 붙어 있는 게임 태그는 결과가 나온 뒤에도 재전송하지 않는다.
// 양쪽 Gain에서 읽기 실패가 2회 이상, 600ms 이상 이어져야 같은 태그를 재무장한다.
bool gameplay_tag_latched = false;
String gameplay_tag_user = "";
bool gameplay_tag_missing = false;
unsigned long gameplay_tag_missing_since_ms = 0;
unsigned int gameplay_tag_miss_count = 0;

void BeginRevivalApproval(unsigned long tagDetectedMs);
void EndRevivalApproval(const char *reason, bool preserveUser = false);
void UpdateRevivalApprovalState();
void PollRevivalApproval();
void ObserveGameplayTag(bool detected);
void AdminCardPollPending();

bool send_nfc_err = false;

// 근접 인식 Dead Zone 대응용 RxGain 전환 (rfid.ino 구현) — GainMode는 currentGain 등
// 내부 상태 변수 타입으로만 쓰이고 함수 매개변수 타입으로는 쓰이지 않는다(ApplyGain은 int를 받음).
// Arduino가 .ino 탭들을 병합할 때 자동 생성하는 함수 프로토타입이 실제 코드보다도 앞에
// 삽입돼서, 커스텀 enum을 매개변수로 쓰면 "타입을 아직 모른다"는 컴파일 에러가 나기 때문.
enum GainMode { GAIN_CONTACT, GAIN_NEAR, GAIN_FAR };

void RfidInit(void);
void RfidLoop(void);
void CardChecking(uint8_t rfidData[32]);
void AdminCardPollReady(void);  // ready 상태(activate_bool=false) 전용 — MMMM 관리자 카드만 인식

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
#define DEFAULT_BRIGHTNESS 50   // 0~255. 이 값만 올리면 전체가 밝아진다.
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
void NeoBlinkPurple(int times);  // "사용 불가" 알림 점멸 — tagger는 보라색, 그 외는 빨간색

//=============================== Solenoid ================================
void SolenoidInit();
void SolenoidOn();
void SolenoidOff();
void SolenoidPulse();  // 상태 전환 순간에만 SOLENOID_PULSE_MS만큼 짧게 통전 후 자동으로 끔
unsigned long SolenoidPulse(unsigned long ms);  // 통전 후 끄고, 실제 HIGH 시각(millis)을 반환

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

void TimerInit();
void TimerRun();
void SetWifiPollInterval(unsigned long ms);
void RfidTimerAssess();
void RfidTagTimerFunc();
void WifiTimerFunc();
void NsecTagTimerFailFunc();
void NsecTagTimerSuccessFunc();

#endif
