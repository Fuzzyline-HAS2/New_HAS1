# HAS1_escape_main 부팅 홈잉 + 날개 위치 플래그 설계

작성일: 2026-09-19
대상: `devices/HAS1_escape_main` (현재 main = Firmware v37)
근거: 노션 이슈 "탈출장치 activate 상태에서 watchdog시 날개가 열린 상태에서 더 열림" (종류 탈출장치, 후순위, 작업 예정)
비범위: RTC 메모리 위치 저장, 개방 측 리미트 스위치 추가, 홈잉을 WiFi 초기화 앞으로 옮기는 순서 변경, 미사용 `EN_PIN`

## 배경

탈출장치 날개는 스테퍼 모터로 열고 닫는다.

- `EscapeClose()`(`stepper_Motor.ino`): 닫힘 방향으로 돌다가 리미트 스위치 `SW_PIN`이 HIGH가 되면 멈춘다. 배선 불량 대비로 최대 1500스텝(`stepsPerRevolution * 15`) 안전장치가 있다. 스위치가 이미 눌려 있으면 while이 한 번도 돌지 않고 즉시 끝난다.
- `EscapeOpen()`: 열림 방향으로 피드백 없이 1000스텝(`stepsPerRevolution * 10`)을 무조건 돈다. 현재 위치를 보지 않는다.

서버 상태는 `has2wifi.Loop(DataChanged)`(2초 폴링)로 들어오고, `DataChanged()`(`wifi.ino`)가 `my`와 `cur`을 비교해 변경분에만 `ActivateFunc()`/`ReadyFunc()` 등을 부른다.

## 원인

이슈의 "watchdog"은 ESP32 하드웨어 WDT가 아니라 서버 플래그다. HAS2_Wifi 라이브러리의 `Loop()`가 `shift_machine["watchdog"] >= 1`을 보면 `watchdog=0`을 쓰고 `ESP.restart()`를 부른다.

재부팅 후 경로:

1. `setup()` → `has2wifi.Setup("badland")` → `ReceiveMine()`이 서버에서 `game_state=activate`를 읽어 `my`에 채운다.
2. `DataChanged()`가 불린다. `cur`은 비어 있으므로 `my["game_state"] != cur["game_state"]`가 참이고 `ActivateFunc()`가 실행된다.
3. `ActivateFunc()` → `EscapeOpen()` → 이미 열린 날개에서 1000스텝을 더 연다.

같은 경로가 전원 재투입, OTA 성공 후 재부팅, Beetle UART 복구 3회 실패 시 `ESP.restart()` 등 모든 재부팅에서 동일하게 발생한다. 서버 watchdog만의 문제가 아니다.

재부팅 외에도 이중 개방 경로가 하나 있다. `HandleMmmmCard()`(`serial_communication.ino`)는 `my["device_state"]`가 activate가 아니면 `ActivateFunc()`를 부른다. `ApplyMmmmState("activate")`가 서버 쓰기에 3회 실패하면 `ReceiveMine()`이 로컬 `device_state`를 ready로 되돌리므로, 날개는 열려 있는데 다음 MMMM 태그가 다시 `EscapeOpen()`을 부른다.

## 설계

원칙: 부팅 시 홈(리미트 스위치)을 먼저 잡고, 그 뒤로는 펌웨어가 날개 위치를 플래그로 기억한다. `EscapeOpen()`은 홈에서만 출발한다.

### A. 날개 위치 플래그

`HAS1_escape_main.h` Step Motor 섹션에 추가:

```cpp
bool wingsOpen = false; // false=홈(리미트 스위치 눌림), true=열림(홈에서 1000스텝)
```

### B. `EscapeOpen()` 스킵

`stepper_Motor.ino`의 `EscapeOpen()` 진입부:

```cpp
if (wingsOpen) {
    Serial.println("[MOTOR] 이미 열림, EscapeOpen 스킵");
    return;
}
```

펄스 루프 완료 후, `"Open Finish"` 출력 직전에 `wingsOpen = true;`.

스킵은 모터만 건너뛴다. `ActivateFunc()`의 MP3(VE1), LED(YELLOW), GameTimer enable, `ptrCurrentMode = TagCount`는 그대로 실행된다. MMMM 쓰기 실패 경로에서 `ApplyMmmmState("activate")` 재시도도 그대로 일어난다.

### C. `EscapeClose()` 플래그 해제

`stepper_Motor.ino`의 `EscapeClose()` 끝, `digitalWrite(RELAY_PIN, LOW)` 다음이자 `"Close Finish"` 출력 직전에 `wingsOpen = false;`.

maxSteps 타임아웃(스위치가 끝내 HIGH가 안 됨)으로 끝난 경우에도 `false`로 둔다. 근거:

- 모터가 돌았다면 닫힘 방향으로 1500스텝은 열림 1000스텝을 넘으므로 기계적으로 홈이거나 그 너머다. 다음 `EscapeOpen()`이 1000스텝 여는 것이 맞다.
- 모터가 아예 안 돌았다면(릴레이·드라이버 불량) 플래그와 무관하게 다음 동작도 움직이지 않는다.

3상태(unknown)는 두지 않는다.

### D. 부팅 홈잉

`HAS1_escape_main.ino` `setup()`에서 `pinMode(RELAY_PIN, OUTPUT); digitalWrite(RELAY_PIN, HIGH);` 직후, `DataChanged();` 직전:

```cpp
// 부팅 홈잉. 재부팅 전 날개 위치를 모르므로 서버 상태를 반영하기 전에
// 리미트 스위치까지 닫아 홈을 잡는다. 닫혀 있었으면 0스텝으로 즉시 끝난다.
Serial.println("[HOME] 부팅 홈잉");
EscapeClose();
```

별도 함수로 감싸지 않는다. 감싸도 로그 한 줄 차이다.

홈잉은 WiFi 연결과 서버 읽기 뒤에 실행된다. WiFi 실패로 `has2wifi.Setup()`이 재부팅을 반복하는 동안은 날개가 열린 채 유지된다. 초기화 순서를 흔들지 않기 위해 이번엔 이 위치를 유지한다(비범위).

### E. 버전

`FIRMWARE_VER`는 소스에서 손대지 않는다. Deploy Firmware 워크플로의 "버전 증가" 단계가 `ci_deploy.py bump`로 `.ino`의 값을 +1 해서 `Firmware vN` 커밋을 브랜치에 push한다. 수동으로 38로 올리면 CI가 39로 만들거나 bump 스크립트와 충돌한다. 현재 소스와 배포된 release 모두 v37이므로 배포 후 v38이 된다.

### 건드리지 않는 파일

`wifi.ino`, `serial_communication.ino`, `Game_system.ino`, `error_recovery.ino`. 기존 `EscapeOpen()`/`EscapeClose()` 호출부는 수정 없이 플래그 보호를 받는다.

## 동작 흐름

| 시나리오 | 흐름 | 결과 |
|---|---|---|
| activate 중 재부팅(watchdog·전원·OTA·복구 재시작) | 홈잉 `EscapeClose()` 약 1000스텝 → `DataChanged()` → `ActivateFunc()` → `EscapeOpen()` 1000스텝 | 정확히 홈에서 1000스텝. 닫힘 약 4초 + 열림 약 4초 |
| ready/setting 중 재부팅 | 홈잉 0스텝 → `ReadyFunc()`/`SettingFunc()` → `EscapeClose()` 0스텝 | 변화 없음 |
| 정상 ready → activate | `EscapeOpen()` 실행, `wingsOpen = true` | 기존과 동일 |
| 정상 activate → ready/escape/player_win | `EscapeClose()` 실행, `wingsOpen = false` | 기존과 동일 |
| MMMM 서버 쓰기 3회 실패 후 재태그 | `ActivateFunc()` → `EscapeOpen()` 스킵 → MP3·LED·타이머·`ApplyMmmmState` 재시도 | 이중 개방 없음 |

## 감수하는 점

activate 중 재부팅이면 날개가 닫힌 뒤 다시 열려 총 약 8초간 움직인다. 서버 상태가 activate이므로 재개방은 필요하고, 위치 확실성과 맞바꾸는 비용이다. 재개방을 생략하는 방식(재부팅 전 위치를 RTC 메모리에 저장)은 모터 동작 중 재부팅 시 위치를 모르는 채 신뢰하게 되고 전원 재투입에 무력하므로 채택하지 않았다.

## 테스트

### Python 미러 (`tests/`)

`state_machine.py`(`EscapeMainSM`)에 추가:

- `self.wings_open = False` 속성.
- `_escape_open()`: `wings_open`이면 `"[MOTOR] EscapeOpen skipped"` 이벤트만 남기고 return. 아니면 `"EscapeOpen"` 이벤트 후 `wings_open = True`.
- `_escape_close()`: `"EscapeClose"` 이벤트 후 `wings_open = False`.
- 기존 `_setting_func`/`_ready_func`/`_activate_func`/`_tag_count`/`data_changed`(player_win)의 직접 `_ev("EscapeOpen")`/`_ev("EscapeClose")` 호출을 위 헬퍼로 교체. 기존 테스트가 보는 `"EscapeOpen"`/`"EscapeClose"` 이벤트 문자열은 유지.
- `boot(game_state, device_state=None)`: `"[HOME] boot homing"` 이벤트 → `_escape_close()` → `data_changed(game_state, device_state)`. `setup()`의 홈잉 후 첫 `DataChanged()`에 대응.

새 파일 `tests/test_wing_position.py`:

1. `boot("activate")` → 이벤트에서 `"EscapeClose"`가 `"EscapeOpen"`보다 먼저 나오고, `"EscapeOpen"`은 정확히 1회, `wings_open is True`.
2. `boot("ready")` → `"EscapeOpen"` 없음, `wings_open is False`.
3. `boot("activate")` 후 `_activate_func()` 재호출 → `"EscapeOpen"` 총 1회, `"[MOTOR] EscapeOpen skipped"` 1회, `wings_open is True`.
4. `boot("activate")` → `data_changed(game_state="ready")` → `data_changed(game_state="activate")` → `"EscapeOpen"` 총 2회(닫은 뒤 다시 열기 허용).

기존 4개 테스트 파일(`test_state_transitions.py`, `test_tag_escape.py`, `test_recovery.py`, `test_edge_cases.py`)이 계속 통과해야 한다.

pytest는 시스템 python에 없다. 스크래치패드에 venv를 만들어 `requirements.txt`의 pytest를 설치해 실행한다. 저장소에는 venv를 커밋하지 않는다.

### 실기 확인 (사용자)

- activate 상태에서 서버 watchdog 재부팅 → 날개가 닫힌 뒤 한 번만 열리는지.
- telnet 로그에 `[HOME] 부팅 홈잉`이 `ACTIVATE` 앞에 찍히는지.
- ready 상태에서 재부팅 → 날개가 움직이지 않는지(홈잉 0스텝).
