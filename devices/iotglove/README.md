# IoT 글러브

TTGO T1과 Beetle ESP32-C3용 1호점 The Origin 펌웨어. Nextion 없이 칩·발각 버튼·4칸 NeoPixel·진동·BLE 위치·배터리를 사용한다. **G1P1~G1P8 총 8대, 16개 보드의 초기 설치와 v3 OTA·새 부팅·Telnet 검증을 완료**했다. 장치별 실측과 재시도 이력은 [설치 현황](docs/ROLLOUT.md)과 [검증 기록](docs/VALIDATION.md)을 따른다. 게임 전체·BLE 위치·배터리 보정은 후속 검증으로 남긴다.

## 파일과 보드

| 위치 | 역할 |
| --- | --- |
| `iotglove.ino`, `application.cpp` | TTGO 초기화, 센서/출력 루프, UART와 순차 OTA·리셋 조정 |
| `game_model.h/.cpp` | 실제 펌웨어와 호스트 테스트가 공유하는 상태 전이·카운트·훈련 규칙 |
| `glove_network.h/.cpp`, `network_policy.h`, `state_policy.h` | first_store 전용 worker, 검증된 스냅샷, 순서가 있는 전송과 응답 확인 |
| `feedback.h`, `feedback_config.h`, `battery.h`, `hardware_config.h` | 비차단 출력·상태별 진동 설정, 배터리 샘플 평균/범위 검사, 실물 설정 |
| `peer_state.h` | Beetle 부팅 식별자·heartbeat·위치 만료·OTA 완료 확인 |
| [iotglove_beetle](iotglove_beetle/README.md) | BLE 위치 수집, GPIO 리셋 감지/WDT, 별도 OTA |
| [공통 UART 라이브러리](../../libraries/IoTGloveProtocol/README.md) | 버전 프레임, 고정 길이 파서, 위치 필터·리셋 상태 처리 |
| [빌드 방법](docs/BUILD.md), [검증 기록](docs/VALIDATION.md) | compile-only 검증, Actions/Release, 실기기 절차 |

## 배선

| 기능 | TTGO | Beetle |
| --- | --- | --- |
| UART TTGO → Beetle | GPIO32 TX | GPIO6 RX |
| UART Beetle → TTGO | GPIO36 RX | GPIO5 TX |
| 리셋 요청 | GPIO12 출력 | GPIO1 입력 (회로도 심벌의 물리 핀 4, 라벨 1) |
| NeoPixel 4개 | GPIO25 | — |
| 생명칩, LOW=장착 | GPIO26 | — |
| 발각 버튼, LOW=눌림 | GPIO27 | — |
| 진동, HIGH=ON | GPIO13 | — |

공통 GND와 실제 구동 회로를 확인한다. GPIO12는 모터 용도로 사용하지 않는다. 리셋은 LOW 대기 → HIGH 150ms → LOW이며 요청 간 30초 간격을 둔다. Beetle은 부팅 후 LOW를 확인해야 재무장하고, 유효 요청 뒤 자기 watchdog feed를 중단해 재부팅한다. GPIO12는 TTGO 부팅 strap 핀이므로 부팅 전부터 LOW가 유지되는 회로가 필요하다.

2026-09-17 사용자 회로도 확인으로 Beetle 리셋 입력을 GPIO3에서 **GPIO1**로 정정했다. 기존 GPIO3 이미지에서는 요청을 감지하지 못했으나, 수정본을 두 보드에 USB 설치한 뒤 `b` 펄스 후 새 boot ID와 일치 PING/HELLO 응답으로 Beetle 재부팅·UART 복구를 확인했다. UART에서 하드웨어 reset cause는 직접 읽지 않았다. 설치 이력과 실측 근거는 [검증 기록](docs/VALIDATION.md)에 구분한다.

## 본게임과 훈련

기본 빌드는 Origin, `IOTGLOVE_TRAINING=1`은 서버에 쓰지 않는 독립 훈련이다. 훈련소는 칩 제거 시 파랑 1칸, 3/6/9초에 2/3/4칸, 조기 재장착은 9초 대기, 발각은 1칸부터 재시작한다. 샘플과 달리 **칩 없는 부팅은 유령**으로 시작한다.

본게임은 등록 MAC으로 서버에서 G1/G2 참가자를 받는다. G9의 서버 훈련 권한과 이 독립 훈련 모드를 섞지 않도록 G9 응답은 본게임에서 수락하지 않는다. `tagger_name`이 가리키는 실제 술래의 `device_state=activate`를 확인한 뒤 포획을 허용한다. 제단 활성화 전 칩 제거를 물리적으로 막을 수는 없으며, 생존자 표시 대신 준비 표시를 유지한다. 잘못 빠진 칩은 재장착해야 한다.

- 유령대기는 봉헌 전 칩 재장착으로 복귀한다. 확정 유령은 `is_sacrificed=1`, `revival_count=4`, `is_open=1`, 실제 칩 장착을 모두 만족해야 복귀한다.
- 소생은 한 칸당 서버 `revival_time`초, 파랑 0~4칸이다. 발각은 0으로 초기화한다. 봉헌 전에는 최대 3으로 제한하고, 봉헌 시 현재 한 칸의 경과시간을 유지한다. 이는 문서의 미정 구간에 적용한 초기 정책이다.
- 재부팅·재동기화는 서버 카운트부터 현재 한 칸을 다시 시작한다. 서버가 칩 1개를 확인하고 실제 칩도 꽂힌 경우 별도 증감 없이 복귀 조건을 다시 검사한다.
- 동적 시간 변경은 완료 칸 수를 유지하고 현재 칸을 다시 시작한다. 미확정 로컬 카운트는 해당 응답을 확인한 뒤 서버 변경을 반영한다.
- 칩 이벤트/역할 변경은 하나씩 전송하고 재조회한다. `life_chip`은 증감 API이므로 응답 불확실 시 재전송하지 않는다. 서버의 봉헌·비정상 생명 복구와 원자적 이벤트 계약은 [서버 조사](docs/SERVER_CONTRACT.md)의 통합 확인 대상이다.

서버 통신은 전용 worker에서 수행한다. 첫 유효 응답 전, 자기 상태 조회 실패/무효 응답, 15초 이상 오래된 상태에서는 게임 쓰기를 멈춘다. 술래 조회 실패는 포획만 차단한다. 연결 복구 시 이전 큐를 재생하지 않는다. 서버에 실제 게임 epoch가 없어 단절 중 게임이 완전히 바뀌었다가 같은 상태로 돌아오는 경우의 식별은 서버 보완이 필요하다.

역할 변경과 `life_chip` 증감은 서버에서 원자적으로 묶이지 않는다. 일부 쓰기만 반영된 경우 로그와 역할·서버 칩 수·물리 칩 상태를 운영자가 대조해 복구해야 하며, 글러브가 증감을 추측해 재시도하지 않는다.

## 장치 상태 표시와 진동 설정

2026-09-18 사용자 확정 기준이다. `game_state`는 게임 진행/OTA 및 서버 watchdog 제한, `device_state`는 아래 장치 표시를 구분하는 데 사용한다.

| 조건 | LED | 칩 유무 |
| --- | --- | --- |
| `device_state=setting` | 하양 | 장착 4칸 / 미장착 3칸 |
| `device_state=ready` | 빨강 | 장착 4칸 / 미장착 3칸 |
| `role=tagger`, `device_state=blink` | 보라 점멸, 500ms마다 ON/OFF | 술래 결정 전 표시 |
| `role=tagger`, `device_state=activate` | 보라 상시 점등 | 활성 술래 표시 |

준비 상태에서도 GPIO26을 계속 읽고 30ms 안정화 후 점등 수를 갱신한다. 칩 유무만으로 준비 중 포획·생명 증감 명령을 보내지 않는다. 종료·탐색·서버 무효/만료 상태의 제한을 우선하며, `game_state=activate`에서 장치 준비 표시를 하더라도 게임 중 OTA 및 서버 watchdog 리셋 제한은 유지한다. 수동 `b` 리셋 명령은 기존 동작을 유지한다.

진동은 [feedback_config.h](feedback_config.h)에서 상태별로 지정한다. 짧게 1회·길게 1회·짧게 2회 및 끄기를 선택할 수 있고 기본 길이는 각각 150ms, 300ms, 150ms ON → 100ms OFF → 150ms ON이다. 서버 필드를 추가하지 않으며 설정 변경은 펌웨어 릴리즈와 OTA로 반영한다.

`feedback_config::Settings`의 해당 초기값을 `Pattern::Short1`, `Long1`, `Short2`, `Off` 중 하나로 바꾼다. 길이는 `shortMs`, `longMs`, `doubleGapMs`에서 조절한다.

| 설정 | 기본 패턴 |
| --- | --- |
| `onSetting`, `onReady`, `onExploration`, `onPlayer` | `Short1` — 짧게 1회 |
| `onGhost`, `onTaggerActive`, `onEnded` | `Long1` — 길게 1회 |
| `onTaggerBlink` | `Short2` — 짧게 2회 |
| `onRemoved` — 칩 제거 이벤트 | `Long1` — 기존 300ms |
| `onFound` — 발각 이벤트 | `Short2` — 기존 150/100/150ms |

상태 변경은 역할·장치 상태·게임 페이즈를 기준으로 감지한다. 동일 응답 폴링, LED 점멸 프레임, 칩 유무와 충전 칸 수 갱신은 추가 상태 진동을 만들지 않는다. 첫 동기화와 재접속은 현재 상태만 기준으로 저장한다. 칩 제거·발각 이벤트가 상태 변경 진동보다 우선하고, 상태 진동이 근접 진동보다 우선한다. OTA·리셋 중 진동은 취소하며 억제가 풀린 뒤 늦게 재생하지 않는다.

## 위치·배터리 설정

Beetle의 [beacon_map.h](iotglove_beetle/beacon_map.h)에 현장 `HAS3:장치명`의 **장치명 → 방**을 추가한다. 현재 기본값은 서버에서 확인한 방 이름/별칭만 포함하므로 실제 제단/생명장치 ID 매핑 전에는 위치가 미확인일 수 있다. 방 접두사를 추측하여 쓰지 않는다. 5초 이상 오래된 위치는 무효이며 서버 위치도 빈 문자열로 주기적으로 지운다.

근접 진동 기본안은 같은 방(`vibe=3`) 1초당 100ms 두 번, 인접(`vibe=1`) 2초당 100ms 한 번이다. 칩/발각 진동이 우선하며 오래된 위치로는 울리지 않는다. 실제 방 경계에서 RSSI 필터와 진동 패턴을 조정한다.

배터리는 `analogReadMilliVolts()`의 보정 ADC 값을 20ms마다 모아 16개를 평균한다. 다음 값을 [hardware_config.h](hardware_config.h)에 실측해 넣어야 보고가 활성화된다.

- `IOTGLOVE_BATTERY_PIN`: GPIO35는 기존 코드의 후보이며 실물 배선을 확인한다.
- `IOTGLOVE_BATTERY_DIVIDER_RATIO`: 실제 분압비 `(R위 + R아래) / R아래`.
- `IOTGLOVE_BATTERY_CALIBRATION`: 멀티미터 비교 보정값.
- `IOTGLOVE_BATTERY_MIN_MV`, `IOTGLOVE_BATTERY_MAX_MV`: 배터리 구성에 맞는 측정 허용 범위.

미설정·0/포화 등 무효 ADC 값·범위 이탈은 전압 보고를 생략한다. 보고는 기존 계약대로 `battery_remaining`에 **V, 소수 2자리**, 최소 60초 간격으로 수행한다. 떠 있는 ADC의 모든 단선을 소프트웨어만으로 판별할 수는 없다. 잔량 %로 변환하지 않는다. ADC 범위/보정 API 근거: [Espressif Arduino ADC](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/adc.html).

## 관리·OTA

Beetle의 초기 설치가 끝난 뒤에는 **TTGO USB Serial 115200 또는 같은 LAN의 TTGO Telnet 포트 23으로 두 보드를 진단**할 수 있다. `telnet <TTGO-IP> 23`으로 접속하면 IP·MAC·TTGO 버전·파티션·부팅 ID와 현재 상태가 출력된다. Telnet에서는 아래 한 글자 명령 뒤에 Enter를 입력한다. USB 명령은 기존처럼 한 글자로 동작한다. Telnet은 인증·암호화 없는 운영 LAN 콘솔이며 동시에 한 클라이언트만 연결한다. 훈련소 오프라인 빌드에서는 Telnet을 시작하지 않는다.

| TTGO 명령 | 확인 내용 |
| --- | --- |
| `s` 또는 `?` | IP·MAC, 서버 valid/fresh·device·phase, GPIO26/27 원시값·디바운스 입력·모델 칩 상태, 역할·device_state·동기화·life_chip·포획 허용·로컬/서버 소생 카운트·봉헌/개방, 마지막 LED/모터 출력과 밝기, 로그 유실량, peer known/online, 펌웨어·파티션·부팅 ID, 마지막 유효 수신/HELLO 경과시간, heartbeat의 freshness·uptime·scan·OTA busy, 위치 freshness/방, UART 송수신 누계 |
| `p` | 현재 PING 요청 ID와 **동일한 ID의 유효 HELLO**를 확인하고 RTT 출력. 자동 PING도 2초마다 같은 방식으로 추적 |
| `b` | GPIO12→GPIO1에 리셋 펄스를 보낸 뒤 최대 15초 동안 새 부팅 ID와 펄스 이후 발급한 PING의 일치 응답을 확인 |
| `u` | 두 보드 순차 OTA 요청 |

`inputs`의 GPIO 원시값은 조회 순간의 HIGH=1/LOW=0이며, 현재 칩·버튼 입력은 LOW를 장착·눌림으로 해석한다. `chip_debounced`/`button_debounced`는 30ms 안정화 후 논리값이고 `chip_model`은 게임 모델이 반영한 장착 여부다. `outputs`는 직전 렌더의 명령 캐시이며 `cache_valid=1`일 때 읽는다. RGB는 밝기 적용 전 값, `lit`은 점등 개수, `brightness8`은 0~255 밝기이며, `motor`는 마지막 GPIO13 출력 명령이다. 실제 발광·진동을 측정한 값은 아니므로 실물 관찰과 함께 비교한다. 상태 조회는 게임 피드백이나 입력을 소비하지 않는다.

`probe.latest=matched`, `matched_fresh=1`은 현재 부팅 ID에 대해 최근 양방향 응답을 확인했다는 뜻이다. HEART, 부팅 시 ID 0 HELLO, 이전 요청의 응답, 1.5초 timeout 뒤 도착한 응답으로 이를 갱신하지 않는다. `online=1`만으로는 양방향 연결을 입증하지 않는다. `heart.fresh=0`이면 uptime/scan/busy는 과거 샘플이며, 서버와 게임 상태에 따라 정상 연결에서도 scan은 0일 수 있다. `rx`/`tx` 누계는 TTGO 부팅 이후 값이고 핀 전압 측정값은 아니다.

`b`의 기준 부팅 ID는 명령 입력 시점이 아닌 **실제 HIGH 펄스 시작 시점의 fresh HELLO**에서 고정한다. OTA나 30초 리셋 간격 때문에 펄스가 대기할 수 있다. 접수 로그와 `reset_pending`/`pulse_high`는 현재 요청 상태이며, `last_pulse_result`/`last_pulse_age_ms`는 마지막으로 실제 출력한 펄스의 결과와 경과시간이다. 새 요청이 대기 중일 때 이전 성공을 새 요청의 성공으로 해석하지 않는다. `reboot_observed`는 그 기준과 다른 부팅 ID를 펄스 이후 PING의 정확한 HELLO 응답에서 확인한 결과다. 기준이 없거나 오래됐으면 `baseline_unknown_not_confirmed`, 기한 내 증명이 없으면 `timeout_not_confirmed`로 표시한다. Beetle v1에는 `reset_reason`이 없어 원인은 unknown으로 남는다. 새 Beetle 펌웨어의 추가 `LOG` 프레임은 실제 `esp_reset_reason()` 숫자와 리셋 요청·OTA 연결/검증/결과 단계를 전달한다. TTGO는 현재 peer 부팅 ID에 맞는 순서의 로그만 받아들이고, PING마다 재전송되는 부팅 원인 스냅샷은 별도로 중복 제거한다. 새 부팅 ID만으로 WDT 원인을 추정하지 않는다.

USB와 Telnet은 독립된 고정 크기 최근 로그 버퍼를 사용한다. 느린 클라이언트의 소켓 처리는 별도 작업에서 비차단 방식으로 수행하며, 출력이 5초간 진행되지 않으면 연결을 닫는다. Telnet 수신은 작업 회차당 64바이트, 송신은 128바이트, 명령은 초당 8개로 제한한다. 상태 출력은 1초에 한 번이고 유실 바이트/거부 명령 수는 `s`로 확인한다. 오래된 버퍼 로그 뒤에 현재 접속 배너와 상태가 이어질 수 있으므로 최신 상태와 freshness를 확인한다. Wi-Fi 연결 로그는 고정된 상태 문구만 표시하며 SSID·비밀번호·HTTP 본문은 전달하지 않는다. Beetle 로그 역시 정해진 이벤트 이름과 숫자만 사용한다.

서버의 `device_state`로 최신 또는 특정 버전을 선택한다.

| `device_state` | 동작 |
| --- | --- |
| `github` | 기존 고정 릴리즈 `iotglove`, `iotglove_beetle` 확인 |
| `github@12:7` | TTGO 버전 12, Beetle 버전 7로 설치 또는 롤백 |
| `github@12` | 두 보드 모두 버전 12로 지정 (`github@12:12`와 같음) |

숫자는 각 스케치의 정수 `FIRMWARE_VER`이다. 두 보드는 독립적으로 버전을 증가시키므로 보통 `TTGO:Beetle`을 각각 지정한다. 지정 버전은 `iotglove-v12`, `iotglove_beetle-v7`처럼 보관된 릴리즈가 있어야 한다. 참조 저장소의 `github_dev/rc/prd` 채널 명령은 사용하지 않는다. 잘못된 명령이나 없는 버전을 최신 버전으로 대신 설치하지 않는다.

게임/탐색 중에는 시작하지 않는다. Beetle의 요청 ID·새 부팅 ID·실행 버전으로 성공 또는 동일 버전 skip을 확인한 후 TTGO를 업데이트한다. 지정 OTA는 Beetle이 **요청 버전과 정확히 일치**해야 진행하며 이전 버전도 허용한다. 실패·timeout은 TTGO 업데이트를 중단한다. 진행 중 다른 버전 명령이 오면 진행 중 목표는 바꾸지 않고 다음 요청으로 보관한다.

Beetle 완료 후 TTGO 시작 전에 게임/탐색으로 전환되면 남은 TTGO 단계를 취소한다. 서버 응답을 기다리는 동안에도 작업 시작 후 5분 안에 대기를 끝내고, 완료 확인 뒤 Beetle의 버전이 바뀌거나 5초간 연결이 끊기면 해당 확인을 무효화한다. 아직 시작하지 않은 TTGO 요청은 Wi-Fi 단절 시 실패 처리한다. 이미 플래시에 쓰기 시작한 작업은 중간에 강제 종료하지 않는다.

버전 지정 경로는 보드·펌웨어 버전·파티션 버전·이미지 HMAC을 담은 `ota.txt`와 그 서명 `ota.sig`를 검증한다. 이미지가 해당 정보와 일치해야 OTA 슬롯을 확정하며, 파티션 버전이 다르면 USB 설치가 필요하다. 기존 `github` 경로는 SecureOTA의 고정 릴리즈 규약을 유지한다. 두 보드 업데이트는 순차 처리이므로 TTGO 단계에서 실패하면 Beetle만 변경된 상태일 수 있다. 원인을 해결하고 같은 버전 쌍을 다시 요청하면 이미 맞는 보드는 skip한다. 재시도는 `device_state=setting`이 읽힌 뒤 다시 원하는 명령으로 설정한다.

버전 보관은 이 기능을 배포한 이후부터 적용되며, 예전에 덮어쓴 바이너리는 복원하지 않는다. 두 보드의 최초 USB 설치와 롤백 대상 모두 이 프로토콜을 지원하는 본게임 펌웨어여야 한다. 버전 선택 UI를 서버에 추가한 것은 아니며 기존 서버 관리 기능에서 `device_state` 값을 설정한다. 자세한 배포/보관 규칙은 [빌드 문서](docs/BUILD.md)를 따른다.

서버 `watchdog=1`은 비활성 상태에서 0으로 응답 확인 후 Beetle 리셋 펄스를 보내고 TTGO도 재부팅한다. UART 단절만으로 자동 리셋하지 않는다. 정상 OTA 중 GPIO 리셋은 보류된다.

서명 키는 각 보드의 ignored `secrets.h`와 Release 서명 키를 맞춘다. 최초 두 보드는 동일 `min_spiffs` 파티션으로 USB 설치하며 **현재 펌웨어는 파티션 OTA를 실행하지 않는다**. Actions의 파티션 파일 게시는 기존 배포 규칙상 가능하지만, 글러브 파티션 변경은 USB 절차가 필요하다. 최초 설치·TTGO 복구에 이어 GPIO1 리셋 수정본을 두 보드에 USB 설치하고 원격 재부팅·UART 복구를 확인했다. 두 대상의 GitHub Actions와 Release 서명 검증을 완료했고, G1P1에서 `github@2:2`로 Beetle과 TTGO가 차례로 v2에 재부팅한 뒤 파티션 1·서버 fresh 상태·Telnet 재접속을 확인했다. 최초 업데이트 당시 TTGO v1은 Beetle `LOG`를 소비하지 않았으므로, 업데이트 진행 증거는 UART 기록에, v2의 BOOT 로그·상태·PING 수신 증거는 이후 Telnet 기록에 구분한다. 최신 실기 확인 결과는 [검증 기록](docs/VALIDATION.md)에 남긴다.

개발 현황은 [SW 개발](https://app.notion.com/p/3dd0bd3810bf81eb8bf9f947c26d1333) 한 항목에서 관리한다. 테스트로 확인한 문제는 재현 조건을 갖춘 뒤 별도 서브아이템으로 분리한다.
