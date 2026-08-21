# PRD: HAS1 아이템박스 펌웨어 재설계

> 작성일: 2026-07-22

---

## 1. 목표

| 항목 | 현재 | 목표 |
|---|---|---|
| 아키텍처 | 단일 루프 + 함수 포인터 2개 | 듀얼코어 + 단일 상태 머신 |
| WiFi 블로킹 | 게임 루프 정지 (최대 수 초) | Core 0 격리, 게임 루프 무관 |
| 상태 관리 | `ptrCurrentMode` + `ptrRfidMode` 조합 | `GameState` enum 단일 관리 |
| 연출 블로킹 | `NeoBlink()` delay 2.5초 | 연출 전용 상태로 대체, delay 완전 제거 |
| 타이밍 관리 | 함수마다 `millis()` 계산 산재 | Runnable 플래그 중앙 관리 |
| WiFi 폴링 | 퍼즐 중 타이머 중단 | Core 0 상시 폴링, 중단 없음 |
| 레이어 | 혼재 | AUTOSAR 4계층 준수 |

---

## 2. 소프트웨어 계층 구조

```
┌─────────────────────────────────────────────────────────┐
│  Application Layer          Game_system.ino             │
│  게임 상태 머신, DataChanged 소비                        │
├─────────────────────────────────────────────────────────┤
│  Service Layer       wifi.ino / telnet.ino / ota.ino    │
│  Core 간 Queue, 폴링 주기 관리, 진단 로그, OTA 업데이트 │
├─────────────────────────────────────────────────────────┤
│  ECU Abstraction (HAL)      rfid / motor / encoder 등   │
│  하드웨어 독립 인터페이스                                │
├─────────────────────────────────────────────────────────┤
│  MCAL / Driver              setup(), 라이브러리 직접 호출│
└─────────────────────────────────────────────────────────┘
```

### 2.1 Telnet이 Service Layer인 이유

Telnet은 Log() 함수를 통해 **모든 레이어가 공통으로 사용하는 진단 인프라**다.
AUTOSAR의 DLT(Diagnostic Log and Trace) 모듈이 Service Layer에 속하는 것과 같은 이유다.

- Application 레이어는 Telnet의 존재를 모른다 — `Log("GAME", "...")` 만 호출
- Telnet 연결 여부와 무관하게 Serial 출력은 항상 동작
- 여러 레이어가 공유하지만 어느 레이어도 소유하지 않는 공통 서비스

```
Application  →  Log("GAME", msg)  ┐
HAL          →  Log("RFID", msg)  ├──► Service(Telnet) → Serial + TCP
Service      →  Log("NET",  msg)  ┘
```

---

## 3. 듀얼코어 역할 분담

```
Core 1 (PRO_CPU) — 게임 루프 전용      Core 0 (APP_CPU) — WiFi 전용
────────────────────────────────        ────────────────────────────────
loop()                                  WifiTaskFunc()  [FreeRTOS Task]
├── [1] TickRunnables() → 플래그 갱신   ├── has2wifi.Loop()
├── [2] HAL 실행 (플래그 기반)          │   ├── MaintainWifi()       (재연결 시 수 초 블로킹 가능)
│   ├── MotorHalUpdate    10ms           │   └── HTTP GET shift_machine + ReceiveMine
│   ├── EncoderHalUpdate  20ms           ├── serializeJson → xQueueSend(receiveQueue)
│   └── RfidHalUpdate    200ms           ├── sendQueue 소비 → has2wifi.Send()
├── [3] receiveQueue 소비 (논블로킹)    └── roleRequestQueue 소비
│   └── deserialize → my 갱신                → has2wifi.Receive(tagUser)
│       → DataChanged()                       → xQueueSend(roleResponseQueue, role)
├── [4] GameUpdate()
│   └── ActivateState() — 태그 감지 시 roleRequestQueue 투입 (논블로킹)
│                        — 다음 루프에서 roleResponseQueue 소비
└── [5] TelnetRun()
```

### 3.1 Core 분리 근거

| 작업 | 이유 |
|---|---|
| `ReceiveMine()` → Core 0 | HTTP 작업은 WiFi 코어에, 블로킹이 게임에 무영향 |
| `has2wifi.Send()` → Core 0 | 동일, 송신 지연 수백 ms 허용 |
| `DataChanged()` → Core 1 | `BoxOpen()`, `AllNeoOn()` 등 하드웨어 조작 포함 |
| `my` JSON → Core 1 소유 | race condition 원천 차단 |

---

## 4. HAL Runnable 플래그 스케줄러

### 4.1 설계 원칙

타이밍 계산(언제)과 실제 작업(무엇을)을 분리한다.
AUTOSAR OS Task 활성화(Activate) 개념과 동일하다.

```
TickRunnables()    → "언제" 판단, due 플래그 세팅
HAL 함수들         → "무엇을" 실행, 내부에 millis() 없음
```

### 4.2 Runnable 구조

```cpp
// HAS1_itembox.h
struct Runnable {
    uint32_t periodMs;
    uint32_t lastRun;
    bool     due;
};

Runnable motorR   = {  10, 0, false };
Runnable encoderR = {  20, 0, false };
Runnable blinkR   = {  50, 0, false };  // 연출 애니메이션용
Runnable rfidR    = { 200, 0, false };
```

### 4.3 loop() 실행 구조

```cpp
void loop() {
    // [1] 플래그 갱신 — 가장 먼저, 이 순간의 millis()를 기준으로 통일
    TickRunnables();

    // [2] HAL 실행 — 시간 트리거, 최우선
    if (motorR.due)   MotorHalUpdate();
    if (encoderR.due) EncoderHalUpdate();
    if (blinkR.due)   BlinkHalUpdate();   // 연출 애니메이션 틱
    if (rfidR.due)    RfidHalUpdate();

    // [3] 서버 수신 소비 — 이벤트 트리거, HAL 처리 후 (WiFi 폴링은 항상 계속)
    char jsonBuf[512];
    if (xQueueReceive(receiveQueue, jsonBuf, 0)) {
        deserializeJson(my, jsonBuf);
        DataChanged();   // 상태 무관, 모든 서버 이벤트 처리
    }

    // [4] 게임 상태 머신
    GameUpdate();

    // [5] 진단 로그 미러링
    TelnetRun();
}

void TickRunnables() {
    uint32_t now = millis();
    auto tick = [&](Runnable& r) {
        r.due = (now - r.lastRun >= r.periodMs);
        if (r.due) r.lastRun = now;
    };
    tick(motorR); tick(encoderR); tick(blinkR); tick(rfidR);
}
```

### 4.4 HAL 함수는 타이밍 모름

```cpp
// 기존: 함수 내부에 millis() 산재
void RfidHalUpdate() {
    if (millis() - lastCheck < 200) return;  // ← 타이밍이 함수 안에
    lastCheck = millis();
    ...
}

// 목표: 함수는 작업만, 타이밍은 Runnable이 전담
void RfidHalUpdate() {
    // millis() 없음 — rfidR.due가 true일 때만 호출됨을 보장
    ...
}
```

### 4.5 "동시에 due"일 때 우선순위

```
우선순위 높음 ▲
              │  [2] HAL Runnable  — 시간 트리거, 하드웨어 마감 있음
              │  [3] 서버 수신 소비 — 이벤트 트리거, 수 ms 지연 허용
              │  [4] GameUpdate    — 상태 처리, 매 loop 실행
우선순위 낮음 ▼  [5] TelnetRun    — 진단, 지연 무방
```

AUTOSAR 관점: **시간 트리거(HAL) > 이벤트 트리거(서버 수신)**

### 4.6 DataChanged() 실행 시간 제약

`DataChanged()`는 상태 전환만 수행하고 `delay()`를 포함하지 않아야 한다.
진입 시 1회 실행 동작(LED, 모터 시작)은 논블로킹 호출만 허용.

```cpp
// 금지
void DataChanged() { delay(2000); }

// 허용: ChangeGameState() 안에서 논블로킹 호출만
void ChangeGameState(GameState next) {

    // ── Exit Action: 현재 상태 정리 ──────────────────
    switch (currentState) {
        case GAME_PUZZLE:
        case GAME_PAUSED:
            vibrationOff();   // HAL
            EncoderDisable(); // HAL — 내부에서 detachInterrupt
            GameTimer.stop();
            break;
        case GAME_CORRECT_ANIM:
        case GAME_WRONG_ANIM:
        case GAME_ITEM_FAIL_ANIM:
            blinkCnt = 0;
            break;
        default:
            break;
    }

    // ── Entry Action: 다음 상태 초기화 ──────────────
    switch (next) {
        case GAME_SETTING:
            NeoSetAll(WHITE);  boxOpen();  answerCnt = 0;
            break;
        case GAME_READY:
            NeoSetAll(RED);    boxClose(); answerCnt = 0;
            break;
        case GAME_ACTIVATE:
            NeoSetAll(YELLOW); boxClose(); answerCnt = 0;  EncoderReset();
            break;
        case GAME_PUZZLE:
            NeoSetAll(BLUE);
            lastButtonPressed = isEncoderButtonPressed();  // 진입 순간 버튼 상태 동기화
            rfidLastSeenTime  = millis();
            EncoderEnable();   // HAL — 내부에서 attachInterrupt
            GameEventSend("device_state", "solving");  // 서버에 퍼즐 진행 중 상태 보고
            break;
        case GAME_PAUSED:
            NeoSetAll(YELLOW);
            pauseStartTime = millis();
            GameEventSend("device_state", "activate");
            break;
        case GAME_CORRECT_ANIM:
        case GAME_WRONG_ANIM:
        case GAME_ITEM_FAIL_ANIM:
            blinkCnt = 0;  ledOn = false;
            blinkR.periodMs = 250;  // 점멸 주기 250ms
            blinkR.lastRun  = 0;    // 진입 즉시 첫 점멸
            break;
        case GAME_BOX_OPENING:
            NeoSetAll(GREEN);  boxOpen();
            break;
        case GAME_BOX_OPEN:
            break;  // 현재 하드웨어 미도달 (내부 RFID 미설치)
        case GAME_USED:
        case GAME_DONE:
            NeoSetAll(BLUE);  boxOpen();
            break;
    }

    gameState = next;
}
```

### 4.7 퍼즐 중 서버 수신 처리 (WiFi 폴링 상시 유지)

Core 0은 퍼즐 중에도 WiFi 폴링을 멈추지 않는다.
Core 1은 receiveQueue를 항상 소비하고, `DataChanged()`는 **상태에 무관하게 모든 서버 이벤트를 처리**한다.

GM이 퍼즐 도중 `setting` / `activate` 등을 내려도 즉시 반영되는 것이 올바른 동작이다.
필터가 불필요한 이유: 기존 코드(`WifiTimer.deleteTimer()`)가 폴링을 끊은 이유는 서버 명령을 막으려는 의도가 아니라, WiFi TX 전류와 모터/진동 전류가 겹쳐 발생하던 **brownout** 방지였다. `has2wifi.Send()`가 Core 0으로 분리되면 Core 1 게임 루프와 전류 피크가 물리적으로 겹치지 않으므로 이 문제가 해소된다.

---

## 5. NeoBlink delay 제거

### 5.1 문제

```cpp
// 현재: 5번 × 250ms × 2 = 2.5초 블로킹
// 주석에도 "연출 중 입력 무시 의도"라고 명시돼 있음
NeoBlink(NEO_ENCODER, GREEN, 5, 250);
rfidLastSeenTime = millis();  // 블로킹 동안 오탐 방지 패치
```

블로킹으로 입력을 막으려 했지만, 상태 머신에서는 **상태 자체가 입력을 막는다**.
GAME_CORRECT_ANIM 상태에서는 PuzzleState()가 호출되지 않으므로 별도 블로킹 불필요.

### 5.2 해결: 연출 전용 상태

```
정답 판정 → ChangeGameState(GAME_CORRECT_ANIM)  // delay 없이 즉시 리턴
                    │ entry: blinkR.periodMs = 250, blinkCnt = 0, ledOn = false
                    │
            blinkR.due(250ms)마다 LED on/off 토글
                    │ ledOn 전환 시 blinkCnt++ (ON 때만 카운트)
                    │
            blinkCnt >= 5 완료 → answerCnt 체크
                    ├── 미완료: ChangeGameState(GAME_PUZZLE)
                    └── 완료:   PuzzleSolved() → GAME_BOX_OPENING
                    exit: blinkR.periodMs = 50 (기본 복원)
```

### 5.3 CorrectAnimState() / WrongAnimState()

```cpp
// neopixel.ino — BlinkHalUpdate() 는 빈 훅
// ANIM 상태 함수가 blinkR.due 를 직접 읽어 LED를 제어
void BlinkHalUpdate() {}

// Game_system.ino — blinkCnt, ledOn 은 파일 스코프 변수
// (ChangeGameState entry/exit 에서 초기화하므로 static 불필요)
void CorrectAnimState() {
    if (!blinkR.due) return;
    ledOn = !ledOn;
    NeoSet(NEO_ENCODER, ledOn ? GREEN : BLACK);
    if (ledOn) blinkCnt++;
    if (blinkCnt < 5) return;
    if (answerCnt >= modeValue[RANGE][ANSWER_CNT])
        PuzzleSolved();
    else
        ChangeGameState(GAME_PUZZLE);
}

void WrongAnimState() {
    if (!blinkR.due) return;
    ledOn = !ledOn;
    NeoSet(NEO_ENCODER, ledOn ? RED : BLACK);
    if (ledOn) blinkCnt++;
    if (blinkCnt < 5) return;
    ChangeGameState(GAME_PUZZLE);
}
```

### 5.4 delay() 잔존 현황

| 위치 | delay | 처리 방침 |
|---|---|---|
| `dfplayer.ino` | 2000ms | setup() 안 — 허용 (부팅 1회, 실시간 무관) |

> `neopixel.ino` 의 `NeoBlink()` 250ms×10 블로킹은 ANIM 상태로 완전 대체됨.

---

## 6. 코어 간 통신 (Queue)

### 6.1 Queue 정의

```cpp
struct SendMsg {
    char deviceName[32];
    char key[32];
    char value[64];
};

QueueHandle_t sendQueue;          // Core1 → Core0  (게임 이벤트 → HTTP POST)
QueueHandle_t receiveQueue;       // Core0 → Core1  (서버 데이터 → 상태 전환)
QueueHandle_t roleRequestQueue;   // Core1 → Core0  (tagUser → Receive 요청, depth=1)
QueueHandle_t roleResponseQueue;  // Core0 → Core1  (role 결과 → ActivateState 응답, depth=1)
```

### 6.2 수신 경로

```
Core0: ReceiveMine() 완료
     → serializeJson(my_raw, buf)   // Core0 임시 my → 문자열화
     → xQueueSend(receiveQueue)

Core1: xQueueReceive(receiveQueue)  // 논블로킹 (0 timeout)
     → deserializeJson(my, buf)     // Core1에서만 my 역직렬화
     → DataChanged()                // 하드웨어 조작은 여기서만
```

### 6.3 role 조회 비동기 경로

`HTTPClient http`는 전역 싱글턴이며 thread-safe하지 않다.
Core1에서 `has2wifi.Receive()`를 직접 호출하면 Core0의 `has2wifi.Loop()` HTTP 트랜잭션과 충돌한다.
따라서 **모든 `has2wifi.*` 호출은 Core0 `WifiTaskFunc` 안에서만** 한다.

```
Core1: ActivateState() → 태그 감지 + tagUser 추출
     → WifiRequestPlayer(tagUser)          // roleRequestQueue 투입, 즉시 리턴
     → return (다음 loop() 대기)

Core0: xQueueReceive(roleRequestQueue)
     → has2wifi.Receive(tagUser)            // HTTP GET — Core0 전용 http 객체, 안전
     → xQueueSend(roleResponseQueue, role)

Core1: ActivateState() 다음 호출 → WifiPollPlayerRole()
     → xQueueReceive(roleResponseQueue, 0) // 논블로킹
     → role == "player"  → ChangeGameState(GAME_PUZZLE)
     → role != "" && !="player" → 무시, pendingTagUser 초기화
     → role == ""        → 아직 응답 없음, 다음 loop 재시도
```

**Service 인터페이스 (wifi.ino)**:
```cpp
void   WifiRequestPlayer(const char* tagUser);  // roleRequestQueue 투입, 논블로킹
String WifiPollPlayerRole();                     // roleResponseQueue 폴링, 논블로킹
                                                 // 응답 없으면 "" 반환
```

### 6.4 송신 경로

```
Core1: 게임 이벤트 발생
     → GameEventSend("device_state", "activate")  // 논블로킹 큐 삽입, 즉시 리턴
     → GameEventSend("device_state", "used")

Core0: xQueueReceive(sendQueue)
     → has2wifi.Send(...)          // HTTP POST (블로킹 OK)
```

> **"solving" 송신**: 3호점에서는 퍼즐 중 `WifiTimer`를 끊기 위해 `"solving"` 상태를 서버에 전송했다.
> HAS1은 Core0/1 분리로 WiFi 폴링이 항상 유지되므로 타이머 중단 목적은 불필요하지만, 서버가 퍼즐
> 진행 상태를 파악할 수 있도록 GAME_PUZZLE 진입 시(ChangeGameState entry action) 그대로 전송한다.

### 6.5 `my` JSON 소유권 규칙

`my` 객체는 Core1 만 읽고 쓴다.
Core0은 직렬화 문자열로만 주고받는다 → race condition 원천 차단.

---

## 6.6 wifi.ino 구현 계획

### 역할 분리 원칙

`updated_itembox`의 `Wifi.ino`는 `DataChanged()` + `SettingFunc()` + `ActivateFunc()` 등 Application 로직을 직접 담고 있었다.
새 설계에서는 **`wifi.ino`는 HTTP 운반만** 담당하고, 서버 이벤트 해석은 `Game_system.ino`로 이전한다.

```
wifi.ino (Service Layer)          Game_system.ino (Application Layer)
──────────────────────────        ──────────────────────────────────────
WifiInit()                        DataChanged()       ← wifi.ino에서 이전
WifiTaskFunc() [Core 0]           ChangeGameState()
GameEventSend()                   각 상태 함수들
```

Service Layer가 `ChangeGameState()`를 직접 호출하면 상향 호출(Service → Application) 위반이므로,
`DataChanged()`는 반드시 Application Layer(`Game_system.ino`)에 위치한다.

### wifi.ino 전체 구조

```cpp
// ── Queue 선언 ───────────────────────────────────────────────────────────
struct SendMsg { char deviceName[32]; char key[32]; char value[64]; };
QueueHandle_t sendQueue;          // Core1 → Core0  (게임 이벤트 → HTTP POST)
QueueHandle_t receiveQueue;       // Core0 → Core1  (서버 데이터 → DataChanged)
QueueHandle_t roleRequestQueue;   // Core1 → Core0  (tagUser → Receive 요청)
QueueHandle_t roleResponseQueue;  // Core0 → Core1  (role 결과 반환)

// ── Core 0 태스크 — 모든 has2wifi.* 호출은 여기서만 ─────────────────────
// HTTPClient http 는 전역 싱글턴(비스레드세이프) → Core0 독점 사용
void WifiTaskFunc(void*) {
    for (;;) {
        has2wifi.Loop();  // MaintainWifi + HTTP GET shift_machine + (ReceiveMine 포함)

        // shift_machine >= 1 이면 Loop()가 ReceiveMine()을 이미 호출함
        // → my 갱신됨 → 직렬화 후 Core1으로 전달
        if ((int)shift_machine["shift_machine"] >= 1) {
            char buf[512];
            serializeJson(my, buf, sizeof(buf));
            xQueueSend(receiveQueue, buf, 0);
        }

        // role 조회 요청 처리 (ActivateState에서 투입)
        char tagUserBuf[32];
        if (xQueueReceive(roleRequestQueue, tagUserBuf, 0)) {
            has2wifi.Receive(tagUserBuf);          // HTTP GET — Core0 전용, 안전
            char roleBuf[32];
            strlcpy(roleBuf, (const char*)tag["role"], 32);
            xQueueSend(roleResponseQueue, roleBuf, 0);
        }

        // Core1의 송신 요청 소비 → HTTP POST
        SendMsg msg;
        while (xQueueReceive(sendQueue, &msg, 0))
            has2wifi.Send(msg.deviceName, msg.key, msg.value);
    }
}

// ── Core1 Service 인터페이스 ─────────────────────────────────────────────
// GameEventSend: 논블로킹 송신 (Core1 → Core0 sendQueue)
void GameEventSend(const char* key, const char* value) {
    SendMsg msg;
    strlcpy(msg.deviceName, myDoc["device_name"] | "", 32);
    strlcpy(msg.key,   key,   32);
    strlcpy(msg.value, value, 64);
    if (xQueueSend(sendQueue, &msg, 0) != pdTRUE)
        Log("GAME", String("event DROPPED: ") + key + "=" + value);
}

// WifiRequestPlayer: role 조회 요청 투입 — 논블로킹, 즉시 리턴
void WifiRequestPlayer(const char* tagUser) {
    char buf[32];
    strlcpy(buf, tagUser, 32);
    xQueueOverwrite(roleRequestQueue, buf);  // 이전 미처리 요청 덮어씀
}

// WifiPollPlayerRole: role 조회 결과 폴링 — 논블로킹
// 응답 있으면 role 문자열 반환, 없으면 "" 반환
String WifiPollPlayerRole() {
    char roleBuf[32];
    if (xQueueReceive(roleResponseQueue, roleBuf, 0)) return String(roleBuf);
    return "";
}

void WifiInit() {
    sendQueue         = xQueueCreate(8, sizeof(SendMsg));
    receiveQueue      = xQueueCreate(4, 512);
    roleRequestQueue  = xQueueCreate(1, 32);   // depth=1, 최신 요청만 유효
    roleResponseQueue = xQueueCreate(1, 32);
    // badland 외 AP 이력 제거 — 저장된 SK_DA20_2.4G 시도 방지
    Preferences prefs; prefs.begin("has2wifi", false);
    prefs.remove("last_ssid"); prefs.remove("last_pw"); prefs.end();
    has2wifi.Setup("badland");
    has2wifi.Send((const char*)my["device_name"], "esp_version", "30");
    xTaskCreatePinnedToCore(WifiTaskFunc, "wifi", 8192, NULL, 1, NULL, 0);
}
```

### DataChanged() 구현 (Game_system.ino)

서버 상태 → `GameState` 매핑 전체가 여기에 집중된다.
`updated_itembox`의 `SettingFunc()` / `ActivateFunc()` / `ReadyFunc()` 역할을 `ChangeGameState()` 호출로 대체한다.

```cpp
// Game_system.ino — myDoc: Core1에서 xQueueReceive 후 역직렬화한 최신 서버 상태
void DataChanged() {
    static String prevGameState   = "";
    static String prevDeviceState = "";

    // ── game_state 처리 ──────────────────────────────────────────────────
    String gs = myDoc["game_state"] | "";
    if (gs != "" && gs != prevGameState) {
        if      (gs == "setting")  ChangeGameState(GAME_SETTING);
        else if (gs == "ready")    ChangeGameState(GAME_READY);
        else if (gs == "activate") ChangeGameState(GAME_ACTIVATE);
        prevGameState = gs;
    }

    // ── device_state 처리 ────────────────────────────────────────────────
    String ds = myDoc["device_state"] | "";
    if (ds != "" && ds != prevDeviceState) {
        if      (ds == "activate")    ChangeGameState(GAME_ACTIVATE);
        else if (ds == "open")        ChangeGameState(GAME_BOX_OPENING);
        else if (ds == "close")       boxClose();
        else if (ds == "used")        ChangeGameState(GAME_USED);
        else if (ds == "repaired_all" ||
                 ds == "player_win"   ||
                 ds == "player_lose") ChangeGameState(GAME_DONE);
        // "solving" : HAS1 미사용 — Core0/1 분리로 WiFi 타이머 불필요
        // "github"  : OTA 트리거 — Core0 WifiTaskFunc에서 처리 예정
        prevDeviceState = ds;
    }

    // ── 설정값 갱신 (상태 전환과 무관하게 항상 적용) ─────────────────────
    UpdatePuzzleAnswers();  // puzzle_answer_1~5, puzzle_reset_time
    UpdateBrightness();     // brightness → NeoSetBrightness()
    // UpdateLanguage() 없음 — Nextion 미설치, DFPlayer만 존재
}
```

### 전체 데이터 흐름 요약

```
[Core 0]  Loop() → shift_machine >= 1 → ReceiveMine()
                → serializeJson(my, buf) → xQueueSend(receiveQueue)

[Core 1]  xQueueReceive(receiveQueue)
               → deserializeJson(myDoc, buf)  // Core 1만 myDoc 소유
               → DataChanged()                // Game_system.ino
                    ├─ game_state  변경 → ChangeGameState()
                    ├─ device_state 변경 → ChangeGameState()
                    └─ 설정값 갱신 → UpdatePuzzleAnswers(), UpdateBrightness()

[Core 1]  게임 이벤트 (일시정지, 아이템 수령 등)
               → GameEventSend("device_state", "activate")
               → GameEventSend("device_state", "used")
               → xQueueSend(sendQueue)    // 논블로킹

[Core 0]  xQueueReceive(sendQueue)
               → has2wifi.Send()          // HTTP POST, 블로킹 OK
```

---

## 7. 게임 상태 머신

### 7.1 상태 정의

```cpp
enum GameState {
    // ── 서버 제어 ──────────────────────────────────────────────────────────
    GAME_SETTING,        // game_state=setting.  초기화, 박스 열림 (LED: WHITE)
    GAME_READY,          // game_state=ready.    외부 RFID 스캔, 태그 시 경고만 (LED: RED)
                         // 진입 시 boxClose() — 닫히는 동안 RFID 스캔은 계속 (MotorHalUpdate 백그라운드)
    GAME_ACTIVATE,       // game_state=activate. 외부 RFID 스캔 대기 (LED: YELLOW)
                         // 진입 시 boxClose()
                         // ActivateState()에서: 태그 감지 → CheckingPlayers() → role=="player" → GAME_PUZZLE
                         // tagger/ghost/기타 role → 무시

    // ── 퍼즐 진행 ──────────────────────────────────────────────────────────
    GAME_PUZZLE,         // 퍼즐 진행 중, 외부 태그 유지 필요 (LED: BLUE)
    GAME_PAUSED,         // 태그 이탈 일시정지, 재태그 대기 (LED: YELLOW)
                         // puzzleResetTime 초과 시 GAME_ACTIVATE 복귀
    GAME_CORRECT_ANIM,   // 정답 연출, blinkR 250ms 점멸 5회 (LED: GREEN 점멸)
                         // 완료 후 → GAME_PUZZLE(미완료) 또는 GAME_BOX_OPENING(완료)
    GAME_WRONG_ANIM,     // 오답 연출, blinkR 250ms 점멸 5회 (LED: RED 점멸)
                         // 완료 후 → GAME_PUZZLE 복귀

    // ── 박스 상태 ──────────────────────────────────────────────────────────
    GAME_BOX_OPENING,    // 모터 동작 중 (LED: GREEN)
                         // isBoxOpened() 감지 즉시 → GAME_USED + "used" 서버 보고
                         // (퍼즐 완료 경로 / 서버 device_state="open" 경로 공통)
    GAME_BOX_OPEN,       // 현재 하드웨어에서 미도달 (내부 RFID 미설치)
                         // 미래 내부 RFID 추가 시 활용
    GAME_ITEM_FAIL_ANIM, // 현재 하드웨어에서 미도달 (내부 RFID 미설치)

    // ── 종료 ───────────────────────────────────────────────────────────────
    GAME_USED,           // 아이템 수령 완료, 모든 입력 차단 (LED: BLUE)
                         // 진입 경로: BoxOpeningState(물리 개방) 또는 서버 device_state="used"
    GAME_DONE,           // repaired_all / player_win / player_lose
                         // 단일 상태, LED: BLUE, 모든 입력 차단
};
```

> **GAME_DONE 통합 근거**: repaired_all / player_win / player_lose 모두 진행 동작이 동일(입력 차단)하고 LED 색도 BLUE로 통일한다.

### 7.2 상태 전환 다이어그램

```
[서버 이벤트 — 언제든지 수신 가능]
  game_state=setting                           ──────────────────────────► GAME_SETTING
  game_state=ready                             ──────────────────────────► GAME_READY
  game_state=activate                          ──────────────────────────► GAME_ACTIVATE
  device_state=activate                        ──────────────────────────► GAME_ACTIVATE
  device_state=open                            ──────────────────────────► GAME_BOX_OPENING
  device_state=used                            ──────────────────────────► GAME_USED
  device_state=repaired_all/player_win/player_lose ──────────────────────► GAME_DONE (종료)

[게임 흐름]
GAME_ACTIVATE
    │ 외부 RFID 태그 감지 (RfidTagPresent + RfidReadTag — HAL)
    │ → tagUser 추출 / "MMMM" → ESP.restart()
    │ → WifiRequestPlayer(tagUser) [roleRequestQueue 투입 — 논블로킹, 즉시 리턴]
    │   ↓
    │  [Core0: has2wifi.Receive(tagUser) → roleResponseQueue]
    │   ↓
    │ 다음 ActivateState() 호출 → WifiPollPlayerRole()
    │      ├─ role == "player" → ChangeGameState(GAME_PUZZLE)
    │      ├─ role != ""       → 무시 (pendingTagUser 초기화)
    │      └─ role == ""       → 아직 응답 없음, 다음 루프 재시도
    ▼
GAME_PUZZLE ──── 태그 이탈 ────► GAME_PAUSED ── 재태그 ──► GAME_PUZZLE
    │                                │ puzzleResetTime 초과
    │                                └─────────────────────────────────► GAME_ACTIVATE
    │
    ├── 오답 판정 ──► GAME_WRONG_ANIM ── 점멸 5회 완료 ──────────────► GAME_PUZZLE
    │
    └── 정답 판정 ──► GAME_CORRECT_ANIM ── 점멸 5회 ──┬─(미완료)──► GAME_PUZZLE
                                                       └─(완료)───► GAME_BOX_OPENING
                                                                          │
[박스 개방 공통 경로]                                                      │
GAME_BOX_OPENING ◄────────────────────────────── device_state="open" ────┘
    │ isBoxOpened() 감지
    │ GameEventSend("device_state","used")  ← 물리 개방 시 서버 보고
    ▼
GAME_USED  (아이템 수령 완료, 종료)
```

> 퍼즐 완료 경로와 서버 `device_state="open"` 경로는 **GAME_BOX_OPENING에서 합류**하여 동일하게 처리된다.
> 박스가 열리면 생존자가 실물 아이템을 수거하므로, 열림 감지 즉시 `"used"` 를 서버에 보고한다.

### 7.3 상태 전환 원칙

- 전환은 `ChangeGameState()` 단 하나를 통해서만
- **Exit action** (현재 상태 정리: interrupt detach, timer stop 등)은 `ChangeGameState()` 내 exit switch에만
- **Entry action** (LED, 타이머 시작, 플래그 초기화)도 `ChangeGameState()` 내 entry switch에만
- 상태 함수는 자기 로직만 — cleanup을 직접 하거나 다른 상태의 진입 조건을 알면 안 됨
- MCAL 직접 접근 금지 — `detachInterrupt()` 대신 `EncoderDisable()` HAL 호출
- `ChangeGameState()` 포함 모든 상태 함수에서 `delay()` 금지

---

## 8. 계층 설계 상세

### 8.1 계층별 역할과 파일 경계

```
┌──────────────────────────────────────────────────────────────────┐
│  Application Layer    Game_system.ino                            │
│  - GameState 전환, 퍼즐 로직                                      │
│  - HAL 함수 호출 허용, Service Layer 함수 호출 허용               │
│  - 라이브러리 객체·핀 번호·색상 배열 직접 접근 금지              │
├──────────────────────────────────────────────────────────────────┤
│  Service Layer        wifi.ino / telnet.ino / ota.ino            │
│  - HTTP 운반, Queue, Log 인프라, OTA 업데이트                     │
│  - Application 함수 직접 호출 금지 (Queue로만 통신)               │
├──────────────────────────────────────────────────────────────────┤
│  ECU Abstraction (HAL)  rfid / motor / encoder / neopixel        │
│  - 하드웨어 독립 인터페이스 제공                                  │
│  - "무엇을 하는가" 만 노출, "어떻게 하는가" 는 숨김               │
│  - Application 함수 호출 금지 (상향 호출 금지)                    │
├──────────────────────────────────────────────────────────────────┤
│  MCAL / Driver        library_and_pin.h + HAL 파일 내부          │
│  - 핀 번호, PWM 채널, 라이브러리 직접 호출                        │
│  - HAL 파일 내부에서만 사용. 상위 레이어에 노출 금지              │
└──────────────────────────────────────────────────────────────────┘
```

### 8.2 하드웨어 교체 시나리오

**모터 드라이버 교체 (BTS7960 → L298N)**
```
변경 범위: liner_motor.ino 내부 MCAL 구현만
변경 없음: boxOpen() / boxClose() / MotorHalUpdate() 함수 시그니처
           Game_system.ino 전혀 손대지 않음
```

**메인보드 교체 (ESP32 핀 변경)**
```
변경 범위: library_and_pin.h 의 #define 핀 번호만
변경 없음: HAL 함수 시그니처, Application 로직
```

**RFID 리더 교체 (PN532 → RC522)**
```
변경 범위: rfid.ino 내부 MCAL 구현만
변경 없음: RfidTagPresent() / RfidReadTag() 시그니처
           Game_system.ino 전혀 손대지 않음
```

### 8.3 Service Layer 구성

| 서비스 | 파일 | 실행 코어 | 트리거 |
|---|---|---|---|
| WiFi 폴링·송수신 | wifi.ino | Core 0 | 2초 주기 FreeRTOS Task |
| Telnet 진단 로그 | telnet.ino | Core 0 | TelnetRun() (매 루프) |
| OTA 펌웨어 업데이트 | ota.ino | Core 0 | `device_state == "github"` 수신 시 |
| Watchdog 재시작 | wifi.ino 내 | Core 0 | `watchdog >= 1` 수신 시 |

OTA 트리거 흐름:
```
Core 0: WiFi 폴링 → shift_machine >= 1 → ReceiveMine()
  → serializeJson → xQueueSend(receiveQueue)
Core 1: xQueueReceive → DataChanged() → device_state == "github"
  → Queue로 OTA_TRIGGER 이벤트 전달 → Core 0에서 ota.check() 실행
    → HTTP 다운로드 + 플래시 쓰기 → ESP.restart()
```

### 8.4 서브시스템별 HAL 공개 인터페이스

Application이 부를 수 있는 함수 목록. 이 목록 외 라이브러리 객체·핀 매크로 접근 금지.

**neopixel.ino**
```cpp
void NeopixelInit();
void NeoSetAll(int colorId);               // 모든 스트립 단색
void NeoSet(int strip, int colorId);       // 특정 스트립 단색
void NeoEncoderUpdate();                   // 엔코더 위치 → LED 표시
void BlinkHalUpdate();                     // 점멸 애니메이션 틱 (Runnable에서 호출)
// colorId: WHITE / RED / GREEN / BLUE / YELLOW / BLACK 등 (enum, Application 가시적)
// strip:   NEO_PN532 / NEO_ENCODER / NEO_INNER 등 (enum, Application 가시적)
// 금지: pixels[] 객체, color[][] 배열 — HAL 내부에서만 사용
```

**liner_motor.ino**
```cpp
void MotorInit();
void boxOpen();
void boxClose();
void MotorHalUpdate();   // 10ms Runnable: 스위치 감지 → 자동 정지
bool isBoxOpened();      // Application이 열림 완료 여부 조회용
// 금지: ledcWrite(), MOTOR_PWMA_PIN 등 — HAL 내부에서만 사용
```

**vibration_motor.ino**
```cpp
void vibration_motor_Init();
void vibrationOn(int strength);    // 0~255
void vibrationOff();
void vibrationSetByEncoder(int answer);   // 엔코더 거리 → 세기 계산 포함
// 금지: ledcWrite(), VIBRATION_RANGE_PIN — HAL 내부에서만 사용
```

**rfid.ino** — 순수 HAL, 하드웨어 접근만
```cpp
void RfidInit();
void RfidHalUpdate();               // 200ms Runnable
                                    // sendCommandCheckAck(0x00) ping → PN532 통신 확인
                                    // startPassiveTargetIDDetection() → 태그 유무
                                    // ntag2xx_ReadPage(7) → 태그 데이터 읽기
                                    // 결과를 내부 캐시에 저장 (Application 직접 접근 금지)
bool RfidTagPresent();              // 캐시 조회 — 하드웨어 접근 없음, loop()마다 안전
bool RfidReadTag(uint8_t data[32]); // 캐시 소비 — 소비 후 dataReady=false
// 금지: nfc 객체, PN532 라이브러리 직접 호출 — HAL 내부에서만 사용
// 내부 RFID 미설치 — Inner 관련 함수 없음
```

**Game_system.ino** — Application, 태그 해석·role 판단 (비동기)
```cpp
// ActivateState() 내 role 조회 흐름 — pendingTagUser로 상태 유지
static String pendingTagUser = "";  // ChangeGameState(GAME_ACTIVATE) entry에서 초기화

void ActivateState() {
    // (A) 응답 대기 중 — 결과 폴링
    if (pendingTagUser != "") {
        String role = WifiPollPlayerRole();   // 논블로킹
        if (role == "player") {
            Log("GAME", "puzzle start by " + pendingTagUser);
            pendingTagUser = "";
            ChangeGameState(GAME_PUZZLE);
        } else if (role != "") {
            Log("GAME", "non-player ignored (" + role + ")");
            pendingTagUser = "";
        }
        return;  // 응답 없으면 다음 루프 재시도
    }
    // (B) 신규 태그 감지
    if (!RfidTagPresent()) return;
    uint8_t data[32];
    if (!RfidReadTag(data)) return;
    String tagUser = "";
    for (int i = 0; i < 4; i++) tagUser += (char)data[i];
    if (tagUser == "MMMM") { ESP.restart(); return; }
    pendingTagUser = tagUser;
    WifiRequestPlayer(tagUser.c_str());  // roleRequestQueue 투입, 즉시 리턴
}
```
> `pendingTagUser`는 `ChangeGameState(GAME_ACTIVATE)` entry action에서 `= ""` 초기화.
> Core0이 `has2wifi.Receive()`를 처리하는 동안 Core1 루프는 블로킹 없이 계속 실행된다.

**encoder.ino**
```cpp
void EncoderInit();
void EncoderHalUpdate();        // 20ms Runnable
void EncoderEnable();           // attachInterrupt 내부 처리 — GAME_PUZZLE 진입 시
void EncoderDisable();          // detachInterrupt 내부 처리 — GAME_PUZZLE 이탈 시
long readEncoderValue();        // 현재 위치 (0~ENCODER_MAX)
bool isEncoderButtonPressed();
void EncoderReset();
// 금지: encoder 객체·attachInterrupt 직접 호출 — HAL 내부에서만 사용
```

### 8.4 HAL 인터페이스 파일 분리

HAL 인터페이스(무엇을 하는가)와 구현(어떻게 하는가)을 파일로 분리한다.
Application은 `.h` 헤더만 include하고 구현 파일은 모른다.
드라이버 교체 시 헤더는 그대로, 구현 파일만 교체한다.

```
hal/
├── motor_hal.h          ← Application이 include하는 공개 계약
├── liner_motor.ino      ← BTS7960 구현 (교체 시 이 파일만 바뀜)
│
├── rfid_hal.h
├── rfid.ino             ← PN532 구현 (RC522로 교체 시 이 파일만)
│
├── neopixel_hal.h
├── neopixel.ino         ← Adafruit_NeoPixel 구현
│
├── encoder_hal.h
├── encoder.ino
│
└── vibration_hal.h
    vibration_motor.ino
```

**motor_hal.h** — Application-visible 계약
```cpp
#pragma once
void MotorInit();
void MotorHalUpdate();    // 10ms Runnable
void boxOpen();
void boxClose();
bool isBoxOpened();
```

**liner_motor.ino** — MCAL 포함 구현 (Application 비노출)
```cpp
#include "motor_hal.h"
#include "library_and_pin.h"   // 핀 번호, PWM 설정 — 이 파일만 include

// BTS7960 전용 로직
void boxOpen() {
    digitalWrite(MOTOR_INA1_PIN, HIGH);
    digitalWrite(MOTOR_INA2_PIN, LOW);
    ledcWrite(MOTOR_PWMA_CHANNEL, MOTOR_SPEED);
}
// L298N으로 바꾼다면 이 구현만 교체, motor_hal.h·Game_system.ino 무변경
```

**neopixel_hal.h** — 색상·스트립 enum 포함 (Application-visible)
```cpp
#pragma once

// Application이 사용하는 추상 식별자 (하드웨어 정보 없음)
enum NeoColor { BLACK, WHITE, RED, GREEN, BLUE, YELLOW };
enum NeoStrip { NEO_PN532, NEO_ENCODER, NEO_INNER };

void NeopixelInit();
void NeoSetAll(NeoColor color);
void NeoSet(NeoStrip strip, NeoColor color);
void NeoEncoderUpdate();
void BlinkHalUpdate();
```

**neopixel.ino** — Adafruit 라이브러리 구현 (Application 비노출)
```cpp
#include "neopixel_hal.h"
#include "library_and_pin.h"

// pixels[], color[][] 는 여기서만 선언 — 절대 헤더에 올라가지 않음
static Adafruit_NeoPixel pixels[3] = { ... };
static int color[][3] = { {0,0,0}, {255,255,255}, ... };

void NeoSet(NeoStrip strip, NeoColor c) {
    lightColor(pixels[strip], color[c]);
}
```

**Game_system.ino** — 헤더만 include, 라이브러리 객체 전혀 모름
```cpp
#include "motor_hal.h"
#include "rfid_hal.h"
#include "neopixel_hal.h"
#include "encoder_hal.h"
#include "vibration_hal.h"

// Adafruit_NeoPixel, BTS7960, PN532 등 어떤 라이브러리도 직접 참조하지 않음
void CorrectAnimState() {
    if (!blinkR.due) return;
    ledOn = !ledOn;
    NeoSet(NEO_ENCODER, ledOn ? GREEN : BLACK);   // 헤더 계약만 사용
    ...
}
```

### 8.5 MCAL 격리 규칙

`library_and_pin.h` 에 핀 번호, 주파수, 해상도 등 하드웨어 상수를 집중한다.
이 파일은 HAL/MCAL 파일에서만 `#include` 한다.

```cpp
// library_and_pin.h — MCAL 상수 전용, Application은 직접 include 금지
#define MOTOR_PWMA_PIN      22
#define MOTOR_INA1_PIN      32
#define MOTOR_INA2_PIN       4
#define MOTOR_FREQ        5000
#define MOTOR_RESOLUTION     8

#define VIBRATION_RANGE_PIN 14
#define PN532_SCK           18
// ...
```

```
include 허용:  liner_motor.ino, vibration_motor.ino, rfid.ino, encoder.ino, neopixel.ino
include 금지:  Game_system.ino, HAS1_itembox.h (전역 헤더에 넣으면 Application에 노출됨)
```

### 8.6 계층 호출 방향 규칙

```
허용:  Application → HAL
허용:  Application → Service  (GameEventSend, WifiRequestPlayer, WifiPollPlayerRole 등)
허용:  HAL        → Service   (Log()만 — 진단 인프라 예외)
금지:  HAL        → Application  (상향 호출)
금지:  Service    → Application  (Queue로만 통신)
금지:  Application → MCAL 직접   (라이브러리 객체, 핀 번호 등)

HAL이 Application에 결과를 알려야 할 때:
  HAL 내부 플래그 세팅 → Application이 폴링
  예: rfid.ino에서 ChangeGameState() 호출      ✗  (HAL→Application 금지)
      rfid.ino에서 has2wifi.Receive() 호출     ✗  (HAL→Service 금지, Log 제외)
      rfid.ino에서 rfid_tagPresent = true 세팅 ✓  (Application이 RfidTagPresent()로 폴링)
      Game_system.ino에서 has2wifi.Receive()   ✓  (Application→Service 허용)
```

### 8.7 설계 결정 메모

| 항목 | 결정 | 근거 |
|---|---|---|
| `has2wifi.*` Core0 독점 | Core1은 Queue로만 요청, 결과는 Queue로 수신 | `HTTPClient http` 전역 싱글턴이 thread-safe하지 않아 동시 호출 시 `no HTTP server` 오류 발생 |
| role 조회 비동기화 | `WifiRequestPlayer` + `WifiPollPlayerRole` 분리 | vTaskSuspend(Core0 태스크 정지)는 "Core0 논블로킹" 목표에 반함 |
| `roleRequestQueue` depth=1 | `xQueueOverwrite` 사용 | 태그 연속 감지 시 최신 요청만 유효, 이전 요청 버림 |

---

## 9. 파일 구성

```
HAS1_itembox/
├── HAS1_itembox.ino      — setup(), loop(), TickRunnables(), Runnable 선언
├── HAS1_itembox.h        — 전역 선언, Queue 핸들, GameState enum, Runnable 구조체
├── library_and_pin.h     — MCAL 전용: 핀 번호, PWM 채널, 주파수 상수 (HAL 파일만 include)
│
│   [Application]
├── Game_system.ino       — 상태 머신, ChangeGameState, DataChanged
│                           (HAL .h 파일만 include, 라이브러리 객체 미참조)
│
│   [Service]
├── wifi.ino              — WifiTaskFunc(Core0), receiveQueue/sendQueue, DataChanged 콜백
├── telnet.ino            — Log() 인프라, Telnet 연결 관리
├── ota.ino               — OTA 펌웨어 업데이트 (device_state=="github" 트리거)
│
│   [HAL — 인터페이스 헤더 + 구현 쌍]
├── motor_hal.h           — boxOpen/Close, MotorHalUpdate, isBoxOpened 선언
├── liner_motor.ino       — BTS7960 구현 (library_and_pin.h + ledcWrite)
│
├── rfid_hal.h            — RfidInit, RfidHalUpdate, RfidTagPresent, RfidReadTag 선언
├── rfid.ino              — PN532 구현 (Adafruit_PN532)
│
├── neopixel_hal.h        — NeoColor/NeoStrip enum, NeoSet, NeoSetAll, BlinkHalUpdate 선언
├── neopixel.ino          — Adafruit_NeoPixel 구현 (pixels[], color[][] 비노출)
│
├── encoder_hal.h         — EncoderInit, EncoderEnable/Disable, readEncoderValue, EncoderReset 선언
├── encoder.ino           — 엔코더 인터럽트 구현
│
├── vibration_hal.h       — vibrationOn/Off/SetByEncoder 선언
└── vibration_motor.ino   — ledcWrite 기반 구현
```

---

## 10. 구현 순서

| 단계 | 작업 | 검증 기준 |
|---|---|---|
| 1 | HAL 인터페이스 완성 (객체 직접 접근 → 함수 래핑) | Game_system.ino에서 라이브러리 객체 미참조 |
| 2 | `GameState` enum (11개) + `ChangeGameState()` + `GameUpdate()` 작성 | 상태 전환 로그 정상 출력 |
| 3 | Runnable 구조체 + `TickRunnables()` 적용 | 각 HAL 함수 내부 millis() 제거 확인 |
| 4 | `GAME_CORRECT_ANIM` / `GAME_WRONG_ANIM` / `GAME_ITEM_FAIL_ANIM` 상태 추가 | NeoBlink delay 제거, 연출 중 입력 무시 확인 |
| 5 | Queue 선언 + `WifiTaskFunc()` Core0 핀닝 | 게임 루프 중 HTTP 블로킹 없음 (Motor 10ms 주기 유지) |
| 6 | `GameEventSend()`로 기존 `has2wifi.Send()` 전부 교체 | 송신 누락 없음 확인 |
| 7 | `DataChanged()` 서버 상태 → `ChangeGameState()` 매핑 완성 | 모든 server 이벤트가 올바른 상태로 전환됨 확인 |
