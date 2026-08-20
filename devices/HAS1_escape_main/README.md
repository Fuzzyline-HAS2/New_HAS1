# HAS1_escape_main

ESP32 기반 탈출장치 메인 컨트롤러입니다.

- 상태 입력: HAS 서버의 `game_state` / `device_state`
- 현장 입력: Beetle UART 태그 패킷
- 구동 출력: 스테퍼 모터, 릴레이, NeoPixel, DFPlayer
- 진단: QC 룰 엔진 + 런타임 오류 복구 계층

## 구조

```text
HAS1_escape_main/
├── HAS1_escape_main.ino           메인 setup / loop
├── HAS1_escape_main.h             전역 상태, recovery 선언
├── error_recovery.ino        런타임 복구
├── Wifi.ino                  서버 상태 반영과 상태 전이
├── Game_system.ino           태그 집계 및 escape 처리
├── serial_Communication.ino  Beetle UART 패킷 처리
├── stepper_Motor.ino         EscapeOpen / EscapeClose
├── neopixel.ino              LED 제어
├── dfplayer.ino              MP3 재생
├── timer.ino                 주기 실행
├── QC/                       QC 룰 엔진
└── tests/                    Python 상태머신 / recovery 테스트
```

## 동작 개요

1. `setup()`에서 WiFi, 타이머, 모터, QC 룰을 초기화합니다.
2. `HAS2_Wifi`가 서버 상태를 받아오면 `DataChanged()`가 `setting`, `ready`, `activate` 전이를 처리합니다.
3. Beetle은 heartbeat 장치가 아니라 이벤트 장치로 취급합니다.
4. `T` 패킷으로 태그가 들어오면 `TagCount()`가 3명 escape를 판단하고 `device_state=escape`를 전송합니다.
5. QC는 이상을 감지해 로그로 보고하고, 실제 복구는 `error_recovery.ino`가 담당합니다.

## 오류 탐지

### 1. QC 룰 기반 탐지

`loop()`에서 `QCEngine::getInstance().tick()`이 자동 실행되며, 이상이 있으면 Serial에 `[WARN]`, `[FAIL]` 로그를 남깁니다.

#### Fast 룰

| ID | 내용 |
|---|---|
| `NET_WIFI_00` | WiFi 연결 끊김 감지 |
| `HW_PIN_01` | NeoPixel / 모터 / 릴레이 / UART 핀 충돌 검사 |
| `HW_GPIO_01` | 출력 핀이 ESP32 입력전용 GPIO(34~39)에 할당됐는지 검사 |
| `LOGIC_TAG_01` | `tagCnt`가 0~3 범위를 벗어나는지 검사 |

#### Slow 룰

| ID | 내용 |
|---|---|
| `SYS_MEM_01` | 힙 메모리 부족 감지 |
| `NET_WIFI_01` | WiFi RSSI 약화 감지 |
| `SYS_RST_01` | Brownout / WDT / Panic 재시작 원인 감지 |
| `LOGIC_STATE_01` | 서버 `game_state` / `device_state` 허용값 검사 |
| `HW_STEPPER_01` | `stepsPerRevolution` 범위 검사 |
| `HW_SW_01` | `setting` / `ready`에서 리미트 스위치 상태 불일치 감지 |
| `HW_RELAY_01` | 릴레이 상태와 `game_state` 불일치 감지 |
| `HW_SW_03` | 리미트 스위치 chatter 감지 |
| `LOGIC_SERIAL_02` | Beetle `T` 패킷 형식 오류 감지 |
| `LOGIC_SERIAL_03` | Beetle 알 수 없는 명령 감지 |
| `LOGIC_TAG_02` | 태그 파싱 실패 누적 감지 |
| `HW_BOOT_01` | ESP32 strapping pin 사용 경고 |
| `HW_GPIO_02` | 부팅 직후 출력 핀 안전 상태 경고 |

주의:

- 기존의 `activate 중 Beetle 무응답` 기반 timeout 룰은 제거했습니다.
- 이 프로젝트에서 Beetle silence는 정상 동작일 수 있으므로, silence는 오류로 간주하지 않습니다.

### 2. 런타임 입력 검증

`serial_Communication.ino`는 Beetle 입력을 이벤트 기반으로 검사합니다.

- 빈 문자열 방어
- 허용 명령만 통과: `W`, `R`, `T`, `B`, `M`
- `T` 패킷은 substring 전에 형식 검증
- 태그 문자열 길이 검사
- `PlayerDetector()`에서 role 미해석 시 parse failure 누적

즉 Beetle 쪽은 아래 3가지만 복구 대상으로 봅니다.

- malformed `T` packet
- unknown command
- tag parse failure

## 오류 복구

복구 로직은 [`error_recovery.ino`](/c:/Users/ok/HAS1_escape_main/error_recovery.ino) 에 있습니다.

### 1. Beetle UART 복구

Beetle silence는 복구 트리거가 아닙니다.

아래 bad event가 누적될 때만 UART 재초기화를 시도합니다.

- unknown command
- malformed `T` packet
- tag parse failure

흐름:

1. bad event 발생
2. `HandleRuntimeRecovery()`가 streak 누적
3. 3사이클 누적 시 `RecoverBeetleConnection()` 실행
4. UART 재초기화 + 오류 카운터 초기화 (`R` × 3 → `W` 재전송)
5. 복구가 3회 이상 반복 실패하면 마지막 수단으로 `ESP.restart()`

### 2. `device_state` 전송 재시도

`device_state=escape`는 `SendDeviceStateWithRetry()`를 통해 전송합니다.

- WiFi 연결 상태 확인
- 연결 중일 때만 송신
- 최대 3회 시도
- 실패 시 로그만 남기고 종료

## 언제 재부팅하나

현재 `ESP.restart()`는 제한적으로만 사용합니다.

1. Beetle이 `M` 명령을 보냈을 때
2. Beetle UART bad-event 복구가 여러 번 반복 실패했을 때

다음 경우에는 재부팅하지 않습니다.

- 일반 QC `[FAIL]`
- 단순 Beetle silence

## 테스트

### Python 테스트

Python 상태머신은 Arduino 로직을 미러링하며, recovery 동작까지 검증합니다.

실행:

```powershell
python -m pytest tests -v -p no:cacheprovider
```

주요 테스트 영역:

- 상태 전이: `setting`, `ready`, `activate`
- 태그 집계와 `device_state=escape` 전송
- Beetle bad event 3회 누적 시 UART recovery
- Beetle silence는 recovery 트리거가 아님

### 수동 확인 포인트

- `setting -> ready -> activate`에서 LED / 릴레이 / 타이머 상태가 맞는지
- malformed `T` packet 3회 후 UART recovery 로그가 나오는지
- unknown command 누적 시 recovery가 동작하는지

## 파일별 역할

- [HAS1_escape_main.ino](/c:/Users/ok/HAS1_escape_main/HAS1_escape_main.ino): 초기화와 QC 룰 등록
- [HAS1_escape_main.h](/c:/Users/ok/HAS1_escape_main/HAS1_escape_main.h): 전역 상태, recovery 선언
- [error_recovery.ino](/c:/Users/ok/HAS1_escape_main/error_recovery.ino): Beetle UART 복구
- [Wifi.ino](/c:/Users/ok/HAS1_escape_main/Wifi.ino): 상태 전이 처리
- [Game_system.ino](/c:/Users/ok/HAS1_escape_main/Game_system.ino): 태그 집계와 escape 처리
- [serial_Communication.ino](/c:/Users/ok/HAS1_escape_main/serial_Communication.ino): Beetle 이벤트 검증
- [stepper_Motor.ino](/c:/Users/ok/HAS1_escape_main/stepper_Motor.ino): EscapeOpen / EscapeClose
- [QC/QC_Rules.h](/c:/Users/ok/HAS1_escape_main/QC/QC_Rules.h): QC 탐지 규칙
- [tests/test_recovery.py](/c:/Users/ok/HAS1_escape_main/tests/test_recovery.py): recovery 검증

## 요약

이 프로젝트의 에러 처리 정책은 다음 한 줄로 요약됩니다.

- 통신 / 논리 오류는 Beetle UART를 재초기화한다.
