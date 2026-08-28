#ifndef QC_RULES_H
#define QC_RULES_H

#include "../library_and_pin.h"
#include "QC_Types.h"
#include <WiFi.h>
#include <esp_system.h>

// 'my' 는 HAS2_Wifi.h 에서 선언된 서버 데이터 JSON 객체

// ---------------------------------------------------------
// [SYS_MEM_01] 남은 힙 메모리 체크
// ---------------------------------------------------------
// 20KB 미만이면 WARN, 10KB 미만이면 FAIL.
// 메모리 누수나 과도한 String 연산이 반복되면 ESP32가 재시작될 수 있습니다.
class QCRule_HeapMemory : public IQCRule {
public:
  String getId() const override { return "SYS_MEM_01"; }
  String getName() const override { return "Free Heap Check"; }
  bool isFastCheck() const override { return false; }

  QCResult check() override {
    uint32_t freeHeap = ESP.getFreeHeap();
    String val = String(freeHeap) + " bytes";
    if (freeHeap < 10000) {
      return QCResult(QCLevel::FAIL, getId(), "Free Heap", ">=10000", val,
                      "메모리 누수 점검 또는 정적 할당 줄이기");
    } else if (freeHeap < 20000) {
      return QCResult(QCLevel::WARN, getId(), "Free Heap", ">=20000", val,
                      "메모리 사용량을 지속 모니터링 필요");
    }
    return QCResult();
  }
};

// ---------------------------------------------------------
// [NET_WIFI_00] WiFi 연결 상태 체크 (Fast)
// ---------------------------------------------------------
// WiFi.status()가 WL_CONNECTED가 아니면 즉시 FAIL.
// 매 루프마다 실행되어 연결 끊김을 즉시 감지합니다.
class QCRule_WifiConnection : public IQCRule {
public:
  String getId() const override { return "NET_WIFI_00"; }
  String getName() const override { return "WiFi Connection Status"; }
  bool isFastCheck() const override { return true; }

  QCResult check() override {
    if (WiFi.status() != WL_CONNECTED) {
      return QCResult(QCLevel::FAIL, getId(), "WiFi Status",
                      "Connected", "Disconnected",
                      "공유기/비밀번호 확인 또는 ESP32 재시작");
    }
    return QCResult();
  }
};

// ---------------------------------------------------------
// [NET_WIFI_01] WiFi 신호 세기(RSSI) 체크
// ---------------------------------------------------------
// -75dBm 미만이면 WARN, -85dBm 미만이면 FAIL.
// 신호가 약하면 서버 통신이 불안정해져 게임 진행에 지장이 생깁니다.
class QCRule_WifiSignal : public IQCRule {
public:
  String getId() const override { return "NET_WIFI_01"; }
  String getName() const override { return "WiFi Signal Strength"; }
  bool isFastCheck() const override { return false; }

  QCResult check() override {
    if (WiFi.status() != WL_CONNECTED) return QCResult();
    int32_t rssi = WiFi.RSSI();
    String val = String(rssi) + " dBm";
    if (rssi < -85) {
      return QCResult(QCLevel::FAIL, getId(), "WiFi RSSI", ">=-85dBm", val,
                      "공유기와 거리를 줄이거나 안테나 점검");
    } else if (rssi < -75) {
      return QCResult(QCLevel::WARN, getId(), "WiFi RSSI", ">=-75dBm", val,
                      "신호 약함, 패킷 손실 가능성 있음");
    }
    return QCResult();
  }
};

// ---------------------------------------------------------
// [SYS_RST_01] 마지막 리셋 원인 체크
// ---------------------------------------------------------
// 부팅 후 한 번만 실행됩니다.
// Brownout(전압 부족)은 FAIL, WDT/Panic(크래시)은 WARN.
// 릴레이/모터 서지 노이즈나 공유 GND 불량이 주요 원인입니다.
class QCRule_ResetReason : public IQCRule {
public:
  String getId() const override { return "SYS_RST_01"; }
  String getName() const override { return "Reset Reason Check"; }
  bool isFastCheck() const override { return false; }

  QCResult check() override {
    static bool reported = false;
    if (reported) return QCResult();

    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_BROWNOUT) {
      reported = true;
      return QCResult(QCLevel::FAIL, getId(), "Last Reset Reason",
                      "No brownout", "ESP_RST_BROWNOUT",
                      "5V/3.3V 전원 확인, 모터 서지 노이즈, 공유 GND 점검");
    }
    bool isWdt = (reason == ESP_RST_WDT || reason == ESP_RST_INT_WDT);
#ifdef ESP_RST_TASK_WDT
    isWdt = isWdt || (reason == ESP_RST_TASK_WDT);
#endif
    if (isWdt) {
      reported = true;
      return QCResult(QCLevel::WARN, getId(), "Last Reset Reason",
                      "No WDT reset", String((int)reason),
                      "루프 블로킹 코드 확인 (EscapeClose while 루프 등)");
    }
    if (reason == ESP_RST_PANIC) {
      reported = true;
      return QCResult(QCLevel::WARN, getId(), "Last Reset Reason",
                      "No panic", "ESP_RST_PANIC",
                      "널 포인터 / 메모리 오염 확인");
    }
    return QCResult();
  }
};

// ---------------------------------------------------------
// [HW_PIN_01] 핀 번호 충돌 체크 (Fast)
// ---------------------------------------------------------
// library_and_pin.h에 정의된 핀들이 서로 겹치지 않는지 확인합니다.
// Neopixel 3개, 스테퍼 모터, 릴레이, DFPlayer, 서브 시리얼 핀 간 충돌 검사.
class QCRule_PinConflict : public IQCRule {
public:
  String getId() const override { return "HW_PIN_01"; }
  String getName() const override { return "Pin Conflict Check"; }
  bool isFastCheck() const override { return true; }

  QCResult check() override {
    // 1. Neopixel 3개 핀이 서로 겹치지 않는지
    if (LINE_NEOPIXEL_PIN == ROUND_NEOPIXEL_PIN ||
        LINE_NEOPIXEL_PIN == ONBOARD_NEOPIXEL_PIN ||
        ROUND_NEOPIXEL_PIN == ONBOARD_NEOPIXEL_PIN) {
      String val = "LINE:" + String(LINE_NEOPIXEL_PIN) +
                   " ROUND:" + String(ROUND_NEOPIXEL_PIN) +
                   " ONBOARD:" + String(ONBOARD_NEOPIXEL_PIN);
      return QCResult(QCLevel::FAIL, getId(), "Neopixel Pin Conflict",
                      "핀 중복 없음", val,
                      "각 Neopixel 스트립에 서로 다른 GPIO 할당");
    }

    // 2. 스테퍼 모터 핀(STEP, DIR, EN)이 Neopixel 핀과 겹치지 않는지
    const int neoPins[3] = {LINE_NEOPIXEL_PIN, ROUND_NEOPIXEL_PIN, ONBOARD_NEOPIXEL_PIN};
    const int motorPins[3] = {STEP_PIN, DIR_PIN, EN_PIN};
    for (int m = 0; m < 3; m++) {
      for (int n = 0; n < 3; n++) {
        if (motorPins[m] == neoPins[n]) {
          return QCResult(QCLevel::FAIL, getId(), "Stepper/Neopixel Pin Conflict",
                          "핀 중복 없음",
                          "STEP/DIR/EN=" + String(motorPins[m]),
                          "스테퍼 핀을 Neopixel 핀과 분리");
        }
      }
    }

    // 3. RELAY_PIN이 모터/Neopixel 핀과 겹치지 않는지
    for (int i = 0; i < 3; i++) {
      if (RELAY_PIN == neoPins[i] || RELAY_PIN == motorPins[i]) {
        return QCResult(QCLevel::FAIL, getId(), "Relay Pin Conflict",
                        "핀 중복 없음", "RELAY=" + String(RELAY_PIN),
                        "릴레이 핀을 다른 GPIO로 변경");
      }
    }

    // 4. DFPlayer TX 핀이 Neopixel / 모터 핀과 겹치지 않는지
    for (int i = 0; i < 3; i++) {
      if (DFPLAYER_TX_PIN == neoPins[i] || DFPLAYER_TX_PIN == motorPins[i]) {
        return QCResult(QCLevel::FAIL, getId(), "DFPlayer TX Pin Conflict",
                        "핀 중복 없음", "DFPLAYER_TX=" + String(DFPLAYER_TX_PIN),
                        "DFPlayer TX 핀을 다른 GPIO로 변경");
      }
    }

    // 5. 서브 시리얼(Beetle) TX/RX가 DFPlayer TX/RX와 겹치지 않는지
    if (HWSERIAL_TX == DFPLAYER_TX_PIN || HWSERIAL_TX == DFPLAYER_RX_PIN ||
        HWSERIAL_RX == DFPLAYER_TX_PIN || HWSERIAL_RX == DFPLAYER_RX_PIN) {
      String val = "SubTX:" + String(HWSERIAL_TX) +
                   " SubRX:" + String(HWSERIAL_RX) +
                   " MP3TX:" + String(DFPLAYER_TX_PIN) +
                   " MP3RX:" + String(DFPLAYER_RX_PIN);
      return QCResult(QCLevel::FAIL, getId(), "Serial Pin Conflict",
                      "UART 핀 중복 없음", val,
                      "Beetle UART와 DFPlayer UART 핀 분리");
    }

    // 6. SW_PIN(리미트 스위치)이 모터/Neopixel 핀과 겹치지 않는지
    for (int i = 0; i < 3; i++) {
      if (SW_PIN == neoPins[i] || SW_PIN == motorPins[i]) {
        return QCResult(QCLevel::FAIL, getId(), "SW Pin Conflict",
                        "핀 중복 없음", "SW=" + String(SW_PIN),
                        "리미트 스위치 핀을 다른 GPIO로 변경");
      }
    }

    return QCResult();
  }
};

// ---------------------------------------------------------
// [HW_GPIO_01] GPIO 입출력 기능 적합성 체크 (Fast)
// ---------------------------------------------------------
// ESP32의 GPIO34~39는 입력 전용(Input-Only)입니다.
// 출력이 필요한 핀(Neopixel, Motor, Relay, DFPlayer TX, Serial TX)이
// 34~39에 할당되어 있으면 FAIL을 반환합니다.
// DFPLAYER_RX_PIN=39, HWSERIAL_RX=18은 입력이므로 GPIO39도 허용됩니다.
class QCRule_GpioCapability : public IQCRule {
public:
  String getId() const override { return "HW_GPIO_01"; }
  String getName() const override { return "GPIO Capability Check"; }
  bool isFastCheck() const override { return true; }

  QCResult check() override {
    auto isInputOnly = [](int pin) { return pin >= 34 && pin <= 39; };

    // 출력이 필요한 핀 목록
    const int outputPins[] = {
        LINE_NEOPIXEL_PIN, ROUND_NEOPIXEL_PIN, ONBOARD_NEOPIXEL_PIN,
        STEP_PIN, DIR_PIN, EN_PIN,
        RELAY_PIN,
        DFPLAYER_TX_PIN,
        HWSERIAL_TX
    };
    const char *outputNames[] = {
        "LINE_NEO", "ROUND_NEO", "ONBOARD_NEO",
        "STEP", "DIR", "EN",
        "RELAY",
        "DFPLAYER_TX",
        "HWSERIAL_TX"
    };
    const int outputCount = sizeof(outputPins) / sizeof(outputPins[0]);

    for (int i = 0; i < outputCount; i++) {
      if (isInputOnly(outputPins[i])) {
        return QCResult(QCLevel::FAIL, getId(),
                        String(outputNames[i]) + " on Input-Only GPIO",
                        "GPIO 0~33", String(outputPins[i]),
                        String(outputNames[i]) + " 핀을 GPIO34~39 범위 밖으로 이동");
      }
    }

    return QCResult();
  }
};

// ---------------------------------------------------------
// [LOGIC_STATE_01] 게임 상태값 유효성 체크
// ---------------------------------------------------------
// 서버에서 받아온 my JSON의 game_state, device_state가
// HAS1_escape_main에서 허용된 값인지 확인합니다.
// 알 수 없는 상태값이 내려오면 서버 설정 오류 또는 로직 버그입니다.
class QCRule_GameState : public IQCRule {
public:
  String getId() const override { return "LOGIC_STATE_01"; }
  String getName() const override { return "Game State Validity"; }
  bool isFastCheck() const override { return false; }

  QCResult check() override {
    if (my.size() == 0) return QCResult(); // 아직 서버 데이터 없음

    auto safeStr = [](JsonVariantConst v) -> String {
      const char *p = v.as<const char *>();
      return p ? String(p) : String("");
    };

    // game_state 허용값: setting / ready / activate / escape / ""
    if (my.containsKey("game_state")) {
      String gState = safeStr(my["game_state"]);
      bool gValid = (gState == "setting" || gState == "ready" ||
                     gState == "activate" || gState == "escape" || gState == "");
      if (!gValid) {
        return QCResult(QCLevel::WARN, getId(), "game_state",
                        "setting/ready/activate/escape", gState,
                        "서버 game_state 설정 확인");
      }
    }

    // device_state 허용값: setting / activate / ready / escape / player_win / player_lose / ""
    if (my.containsKey("device_state")) {
      String dState = safeStr(my["device_state"]);
      bool dValid = (dState == "setting" || dState == "activate" ||
                     dState == "ready" || dState == "escape" ||
                     dState == "player_win" || dState == "player_lose" ||
                     dState == "");
      if (!dValid) {
        return QCResult(QCLevel::WARN, getId(), "device_state",
                        "setting/activate/ready/escape/player_win/player_lose", dState,
                        "서버 device_state 설정 확인");
      }
    }

    return QCResult();
  }
};

// ---------------------------------------------------------
// [LOGIC_TAG_01] 태그 카운트 범위 초과 방지 체크 (Fast)
// ---------------------------------------------------------
// TagCount()에서 집계되는 tagCnt가 0~3 범위를 벗어나면 이상 동작입니다.
// tagCnt가 3을 초과하면 탈출 트리거 로직이 오동작할 수 있습니다.
class QCRule_TagCntBounds : public IQCRule {
public:
  String getId() const override { return "LOGIC_TAG_01"; }
  String getName() const override { return "Tag Count Bounds Check"; }
  bool isFastCheck() const override { return true; }

  QCResult check() override {
    extern int tagCnt;

    if (tagCnt > 3) {
      return QCResult(QCLevel::FAIL, getId(), "tagCnt", "0~3",
                      String(tagCnt),
                      "TagCount() 로직 확인, tagState[] 초기화 누락 의심");
    }
    if (tagCnt < 0) {
      return QCResult(QCLevel::FAIL, getId(), "tagCnt", ">=0",
                      String(tagCnt),
                      "tagCnt 음수 감소 경로 확인");
    }

    return QCResult();
  }
};

// ---------------------------------------------------------
// [HW_STEPPER_01] 스테퍼 모터 설정 유효성 체크
// ---------------------------------------------------------
// stepsPerRevolution이 0이면 모터가 움직이지 않습니다.
// 너무 작으면(< 10) 회전이 부족하고, 너무 크면(> 500) loop 블로킹이 심합니다.
// EscapeOpen()은 stepsPerRevolution*10 스텝을 blocking으로 실행하므로 주의.
class QCRule_StepperConfig : public IQCRule {
public:
  String getId() const override { return "HW_STEPPER_01"; }
  String getName() const override { return "Stepper Motor Config Check"; }
  bool isFastCheck() const override { return false; }

  QCResult check() override {
    extern const int stepsPerRevolution;

    if (stepsPerRevolution <= 0) {
      return QCResult(QCLevel::FAIL, getId(), "stepsPerRevolution",
                      ">0", String(stepsPerRevolution),
                      "stepsPerRevolution을 양수로 설정 (기본값: 100 또는 200)");
    }
    if (stepsPerRevolution < 10) {
      return QCResult(QCLevel::WARN, getId(), "stepsPerRevolution",
                      ">=10", String(stepsPerRevolution),
                      "회전량이 너무 작아 탈출 장치가 열리지 않을 수 있음");
    }
    if (stepsPerRevolution > 500) {
      return QCResult(QCLevel::WARN, getId(), "stepsPerRevolution",
                      "<=500", String(stepsPerRevolution),
                      "EscapeOpen() 실행 시 loop() 블로킹 시간 과다 (최대 " +
                      String((stepsPerRevolution * 10 * 4) / 1000) + "초)");
    }

    return QCResult();
  }
};

// ---------------------------------------------------------
// [HW_SW_01] 리미트 스위치 상태 일관성 체크
// ---------------------------------------------------------
// SW_PIN은 NC(normally-closed) 스위치 + INPUT_PULLUP 구성입니다.
// 모터가 닫힌 위치(EscapeClose 완료) = 스위치 개방 = SW_PIN HIGH.
// setting/ready 상태에서 SW_PIN이 LOW이면 Close가 완료되지 않은 것입니다.
// → 스위치 배선 불량, 모터 탈조, 또는 EscapeClose while 루프 비정상 종료 의심.
class QCRule_LimitSwitch : public IQCRule {
public:
  String getId() const override { return "HW_SW_01"; }
  String getName() const override { return "Limit Switch State Consistency"; }
  bool isFastCheck() const override { return false; }

  QCResult check() override {
    if (my.size() == 0) return QCResult();

    auto safeStr = [](JsonVariantConst v) -> String {
      const char *p = v.as<const char *>();
      return p ? String(p) : String("");
    };

    String gState = safeStr(my["game_state"]);

    // setting/ready = EscapeClose() 완료 상태 = SW_PIN HIGH 이어야 정상
    if ((gState == "setting" || gState == "ready") &&
        digitalRead(SW_PIN) == LOW) {
      return QCResult(QCLevel::WARN, getId(),
                      "SW_PIN (GPIO " + String(SW_PIN) + ")",
                      "HIGH (closed)", "LOW",
                      "리미트 스위치 배선 점검, 모터 탈조 또는 EscapeClose() 미완료 확인");
    }

    return QCResult();
  }
};

// LOGIC_SERIAL_04 (silence 기반 timeout) 제거됨.
// Beetle은 이벤트 장치이므로 무응답은 정상일 수 있음.
// bad event 누적 복구는 error_recovery.ino의 HandleRuntimeRecovery() 참조.

// ---------------------------------------------------------
// [HW_RELAY_01] 릴레이 상태 vs game_state 일관성 체크
// ---------------------------------------------------------
// EscapeOpen()은 RELAY_PIN을 LOW(ON)로, EscapeClose()는 HIGH(OFF)로 설정합니다.
// activate 상태 = 모터 열림 = RELAY_PIN LOW 이어야 정상.
// setting/ready 상태 = 모터 닫힘 = RELAY_PIN HIGH 이어야 정상.
// 불일치는 ActivateFunc/SettingFunc/ReadyFunc 내 EscapeOpen/Close 호출 누락 의심.
class QCRule_RelayState : public IQCRule {
public:
  String getId() const override { return "HW_RELAY_01"; }
  String getName() const override { return "Relay State Consistency"; }
  bool isFastCheck() const override { return false; }

  QCResult check() override {
    if (my.size() == 0) return QCResult();

    auto safeStr = [](JsonVariantConst v) -> String {
      const char *p = v.as<const char *>();
      return p ? String(p) : String("");
    };

    String gState = safeStr(my["game_state"]);
    int relayPin = digitalRead(RELAY_PIN);
    String val = (relayPin == LOW) ? "LOW (ON)" : "HIGH (OFF)";

    // activate = EscapeOpen() 실행 → RELAY_PIN LOW 이어야 정상
    if (gState == "activate" && relayPin == HIGH) {
      return QCResult(QCLevel::WARN, getId(), "RELAY_PIN", "LOW (모터 전원 ON)",
                      val, "ActivateFunc() 에서 EscapeOpen() 호출 여부 확인");
    }

    // setting/ready = EscapeClose() 실행 → RELAY_PIN HIGH 이어야 정상
    if ((gState == "setting" || gState == "ready") && relayPin == LOW) {
      return QCResult(QCLevel::WARN, getId(), "RELAY_PIN", "HIGH (모터 전원 OFF)",
                      val, "SettingFunc/ReadyFunc 에서 RELAY_PIN HIGH 또는 EscapeClose() 확인");
    }

    return QCResult();
  }
};

// ---------------------------------------------------------
// [HW_SW_03] 리미트 스위치 채터링(접촉 불량) 검사
// ---------------------------------------------------------
// SW_PIN을 20회 × 2ms 간격으로 샘플링(40ms)하여 상태 전환 횟수를 세어
// 4회 이상이면 채터링/배선 불량으로 WARN합니다.
// 정상 전환(EscapeClose 완료) = 1회. 채터링 = 수십 회.
class QCRule_SwitchChatter : public IQCRule {
public:
  String getId() const override { return "HW_SW_03"; }
  String getName() const override { return "Limit Switch Chatter Check"; }
  bool isFastCheck() const override { return false; }

  QCResult check() override {
    const int SAMPLES = 20;
    const int CHATTER_THRESHOLD = 4;

    int prev = digitalRead(SW_PIN);
    int transitions = 0;
    for (int i = 0; i < SAMPLES - 1; i++) {
      delayMicroseconds(2000); // 2ms
      int cur = digitalRead(SW_PIN);
      if (cur != prev) transitions++;
      prev = cur;
    }

    if (transitions >= CHATTER_THRESHOLD) {
      return QCResult(QCLevel::WARN, getId(),
                      "SW_PIN (GPIO " + String(SW_PIN) + ") transitions/40ms",
                      "< 4", String(transitions),
                      "리미트 스위치 배선 커넥터 재결선, 접점 산화 또는 케이블 단선 점검");
    }
    return QCResult();
  }
};

// ---------------------------------------------------------
// [LOGIC_SERIAL_02] Beetle 패킷 길이/포맷 검사
// ---------------------------------------------------------
// 'T' 패킷 수신 시 lastBeetleRawPacket에 원문 저장 후 포맷을 검증합니다.
// 정상 형식: "T1:xxxx_T2:xxxx_T3:xxxx" (길이 23, 구분자 위치 고정)
// 형식 불일치 시 WARN → 배선 노이즈, Beetle 펌웨어 불일치 가능성.
class QCRule_BeetlePacketFormat : public IQCRule {
public:
  String getId() const override { return "LOGIC_SERIAL_02"; }
  String getName() const override { return "Beetle Packet Format Check"; }
  bool isFastCheck() const override { return false; }

  QCResult check() override {
    extern String lastBeetleRawPacket;
    if (lastBeetleRawPacket.length() == 0) return QCResult();
    if (lastBeetleRawPacket[0] != 'T') return QCResult(); // T 패킷만 검사

    bool formatOk = (lastBeetleRawPacket.length() >= 23 &&
                     lastBeetleRawPacket[1] == '1' &&
                     lastBeetleRawPacket[2] == ':' &&
                     lastBeetleRawPacket[7] == '_' &&
                     lastBeetleRawPacket[8] == 'T' &&
                     lastBeetleRawPacket[9] == '2' &&
                     lastBeetleRawPacket[10] == ':' &&
                     lastBeetleRawPacket[15] == '_' &&
                     lastBeetleRawPacket[16] == 'T' &&
                     lastBeetleRawPacket[17] == '3' &&
                     lastBeetleRawPacket[18] == ':');

    if (!formatOk) {
      String val = lastBeetleRawPacket;
      if (val.length() > 30) val = val.substring(0, 30) + "...";
      return QCResult(QCLevel::WARN, getId(), "T-Packet Format",
                      "T1:xxxx_T2:xxxx_T3:xxxx", val,
                      "Beetle 펌웨어 패킷 포맷 확인, UART 노이즈 또는 baudrate 불일치 점검");
    }
    return QCResult();
  }
};

// ---------------------------------------------------------
// [LOGIC_SERIAL_03] 허용되지 않은 명령 문자 검사
// ---------------------------------------------------------
// W/R/T/B/M 외 명령이 수신되면 invalidCmdCount가 증가합니다.
// (serial_communication.ino의 else 분기에서 카운트)
// 수신 즉시 WARN → 배선 노이즈, 잘못된 송신 장치 연결 가능성.
class QCRule_InvalidCommand : public IQCRule {
public:
  String getId() const override { return "LOGIC_SERIAL_03"; }
  String getName() const override { return "Invalid Serial Command Check"; }
  bool isFastCheck() const override { return false; }

  QCResult check() override {
    extern int invalidCmdCount;
    // 카운터를 여기서 초기화하지 않음 — HandleRuntimeRecovery/ResetBeetleErrorCounters 담당
    if (invalidCmdCount > 0) {
      return QCResult(QCLevel::WARN, getId(), "Invalid Cmd Count (cumulative)",
                      "0", String(invalidCmdCount),
                      "허용 명령: W/R/T/B/M. Beetle TX 배선 및 펌웨어 명령 포맷 확인");
    }
    return QCResult();
  }
};

// ---------------------------------------------------------
// [LOGIC_TAG_02] 태그 파싱 실패 누적 검사
// ---------------------------------------------------------
// PlayerDetector()에서 tag 길이 이상 또는 role 미해석 시 tagParseErrorCount가 증가합니다.
// 누적 값을 보고합니다. ResetBeetleErrorCounters() 또는 UART 복구 후 초기화됩니다.
class QCRule_TagParseError : public IQCRule {
public:
  String getId() const override { return "LOGIC_TAG_02"; }
  String getName() const override { return "Tag Parse Error Check"; }
  bool isFastCheck() const override { return false; }

  QCResult check() override {
    extern int tagParseErrorCount;
    if (tagParseErrorCount > 0) {
      return QCResult(QCLevel::WARN, getId(), "Tag Parse Errors (cumulative)",
                      "0", String(tagParseErrorCount),
                      "서버 태그 등록 확인, Beetle UART 배선 점검. "
                      "role이 player/tagger 이외이면 서버 미등록 태그입니다.");
    }
    return QCResult();
  }
};

// ---------------------------------------------------------
// [HW_BOOT_01] Strapping 핀 사용 검사 (부팅 1회)
// ---------------------------------------------------------
// ESP32 strapping 핀(GPIO 0/2/4/5/12/15)에 릴레이, 스위치, 모터 등
// 외부 풀업/풀다운이 연결되면 부팅 모드가 변경될 수 있습니다.
// GPIO12(EN_PIN)는 플래시 전압 스트래핑 핀으로 외부 풀업 시 부팅 불가 → FAIL.
// 나머지 strapping 핀 사용 → WARN.
// 현재 프로젝트: SW_PIN=4, EN_PIN=12, DIR_PIN=15 모두 strapping 핀.
class QCRule_StrappingPins : public IQCRule {
public:
  String getId() const override { return "HW_BOOT_01"; }
  String getName() const override { return "Strapping Pin Usage Check"; }
  bool isFastCheck() const override { return false; }

  QCResult check() override {
    static bool reported = false;
    if (reported) return QCResult();
    reported = true;

    // ESP32 strapping 핀 목록
    const int strappingPins[] = {0, 2, 4, 5, 12, 15};
    const int strappingCount = 6;

    struct PinEntry { int pin; const char *name; };
    const PinEntry projectPins[] = {
      {SW_PIN,              "SW_PIN"},
      {EN_PIN,              "EN_PIN"},
      {STEP_PIN,            "STEP_PIN"},
      {DIR_PIN,             "DIR_PIN"},
      {RELAY_PIN,           "RELAY_PIN"},
      {LINE_NEOPIXEL_PIN,   "LINE_NEO"},
      {ROUND_NEOPIXEL_PIN,  "ROUND_NEO"},
      {ONBOARD_NEOPIXEL_PIN,"ONBOARD_NEO"},
      {HWSERIAL_TX,         "HWSERIAL_TX"},
      {HWSERIAL_RX,         "HWSERIAL_RX"},
      {DFPLAYER_TX_PIN,     "DFPLAYER_TX"},
      {DFPLAYER_RX_PIN,     "DFPLAYER_RX"},
    };
    const int projectCount = (int)(sizeof(projectPins) / sizeof(projectPins[0]));

    String conflicts = "";
    bool hasFail = false;

    for (int i = 0; i < projectCount; i++) {
      for (int j = 0; j < strappingCount; j++) {
        if (projectPins[i].pin == strappingPins[j]) {
          conflicts += String(projectPins[i].name) +
                       "(GPIO" + String(projectPins[i].pin) + ") ";
          if (projectPins[i].pin == 12) hasFail = true;
        }
      }
    }

    if (conflicts.length() > 0) {
      String hint = hasFail
        ? "GPIO12(EN_PIN)은 플래시 전압 핀. 외부 풀업 연결 시 1.8V 모드로 부팅 실패 가능"
        : "부팅 시 외부 강제 풀업/풀다운이 없는지 배선 점검";
      return QCResult(hasFail ? QCLevel::FAIL : QCLevel::WARN,
                      getId(), "Strapping Pin Used",
                      "Non-strapping GPIO only", conflicts, hint);
    }
    return QCResult();
  }
};

// ---------------------------------------------------------
// [HW_GPIO_02] 출력 핀 초기 상태 안전성 검사 (부팅 1회)
// ---------------------------------------------------------
// 부팅 직후 RELAY_PIN, DIR_PIN 이 안전한 기본값인지 확인합니다.
// RELAY_PIN: HIGH(OFF)가 정상. LOW면 모터 전원이 켜진 상태로 부팅한 것.
// DIR_PIN  : setup() 에서 초기값 미지정 시 LOW(기본). 방향이 잘못되면 WARN.
// EN_PIN   : 코드에서 OUTPUT 설정 없음 → 모터 드라이버 Enable 상태 불확실 → WARN.
class QCRule_OutputInitSafety : public IQCRule {
public:
  String getId() const override { return "HW_GPIO_02"; }
  String getName() const override { return "Output Pin Boot State Safety"; }
  bool isFastCheck() const override { return false; }

  QCResult check() override {
    static bool reported = false;
    if (reported) return QCResult();
    reported = true;

    // RELAY_PIN: HIGH = 모터 전원 OFF = 안전
    if (digitalRead(RELAY_PIN) == LOW) {
      return QCResult(QCLevel::WARN, getId(),
                      "RELAY_PIN (GPIO " + String(RELAY_PIN) + ") at boot",
                      "HIGH (motor OFF)", "LOW (motor ON)",
                      "setup()에서 digitalWrite(RELAY_PIN, HIGH) 순서 확인. "
                      "StepMotorInit() 이전에 릴레이 핀을 HIGH로 초기화해야 합니다.");
    }

    // EN_PIN: 코드에서 OUTPUT으로 설정하지 않음 → 외부 풀다운이면 모터 활성화 상태
    // pinMode()로 OUTPUT 지정 없이 LOW 상태면 A4988/DRV8825 Enable(모터 동작 중)
    // → 물리적으로 확인 불가하지만 정적 경고로 알림
    return QCResult(QCLevel::WARN, getId(),
                    "EN_PIN (GPIO " + String(EN_PIN) + ") init",
                    "OUTPUT + HIGH (disabled)", "Not configured",
                    "StepMotorInit()에 pinMode(EN_PIN, OUTPUT); digitalWrite(EN_PIN, HIGH); 추가 권장. "
                    "현재 EN_PIN 미초기화 시 드라이버 Enable 상태가 불확실합니다.");
  }
};

#endif // QC_RULES_H
