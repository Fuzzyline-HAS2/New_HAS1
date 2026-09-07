# HAS1_escape_main

ESP32 기반 탈출장치 메인 컨트롤러입니다.

- 상태 입력: HAS 서버의 `game_state` / `device_state`
- 현장 입력: Beetle UART 태그 패킷
- 구동 출력: 스테퍼 모터, 릴레이, NeoPixel, DFPlayer
- 진단: 런타임 오류 복구 계층

## 구조

```text
HAS1_escape_main/
├── HAS1_escape_main.ino      메인 setup / loop
├── HAS1_escape_main.h        전역 상태, recovery 선언
├── error_recovery.ino        런타임 복구
├── wifi.ino                  서버 상태 반영과 상태 전이
├── Game_system.ino           태그 집계 및 escape 처리
├── serial_communication.ino  Beetle UART 패킷 처리
├── stepper_Motor.ino         EscapeOpen / EscapeClose
├── neopixel.ino              LED 제어
├── dfplayer.ino              MP3 재생
├── timer.ino                 주기 실행
└── tests/                    Python 상태머신 / recovery 테스트
```

## 동작 개요

1. `setup()`에서 WiFi, 타이머, 모터를 초기화합니다.
2. `HAS2_Wifi`가 서버 상태를 받아오면 `DataChanged()`가 `setting`, `ready`, `activate` 전이를 처리합니다.
3. Beetle은 heartbeat 장치가 아니라 이벤트 장치로 취급합니다.
4. `T` 패킷으로 태그가 들어오면 `TagCount()`가 3명 escape를 판단하고 `device_state=escape`를 전송합니다.

## 오류 탐지

### 런타임 입력 검증

`serial_communication.ino`는 Beetle 입력을 이벤트 기반으로 검사합니다.

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

복구 로직은 [`error_recovery.ino`](error_recovery.ino) 에 있습니다.

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

- [HAS1_escape_main.ino](HAS1_escape_main.ino): 초기화
- [HAS1_escape_main.h](HAS1_escape_main.h): 전역 상태, recovery 선언
- [error_recovery.ino](error_recovery.ino): Beetle UART 복구
- [wifi.ino](wifi.ino): 상태 전이 처리
- [Game_system.ino](Game_system.ino): 태그 집계와 escape 처리
- [serial_communication.ino](serial_communication.ino): Beetle 이벤트 검증
- [stepper_Motor.ino](stepper_Motor.ino): EscapeOpen / EscapeClose
- [tests/test_recovery.py](tests/test_recovery.py): recovery 검증

## 요약

이 프로젝트의 에러 처리 정책은 다음 한 줄로 요약됩니다.

- 통신 / 논리 오류는 Beetle UART를 재초기화한다.
