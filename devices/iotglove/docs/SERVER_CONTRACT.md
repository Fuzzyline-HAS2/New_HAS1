# Origin 글러브 서버 연동 조사

## 2026-09-18 확정: 물리 칩 절대값과 서버 역할 판정

사용자 결정은 **기존 `life_chip` 필드에 장착 1 / 미장착 0을 저장하고, 펌웨어는 칩 상태만 보고하며 역할 전환은 서버가 처리**하는 것이다. 별도 칩 DB 필드는 추가하지 않는다. 최신 조사 기준 `fuzzyline-core main@8c73ef70`의 기존 `Send&column=life_chip`은 덧셈이므로 0을 보내도 미장착으로 바뀌지 않는다. 이 API는 기존 장치 호환을 위해 유지하고 새 글러브용 절대값 요청을 추가한다. [기존 필드 분류](https://github.com/Fuzzyline-HAS2/fuzzyline-core/blob/8c73ef70d4ce0d938366495aa7b5959e517f0d3d/store/web-server/esp-routes.js#L204), [기존 누적 처리](https://github.com/Fuzzyline-HAS2/fuzzyline-core/blob/8c73ef70d4ce0d938366495aa7b5959e517f0d3d/store/web-server/esp-routes.js#L3600)

| 항목 | 새 계약 |
| --- | --- |
| 요청 | `GET /api/esp?request=SetGloveChip&mac=<STA MAC>&key=<장치명>&value=0 또는 1`, `/has2.php` 별칭도 동일 |
| 대상 | Origin 규칙의 본게임 G1/G2, 등록 MAC/key가 일치하는 `device_type=iotglove` |
| 저장 | 기존 `life_chip`을 정수 0/1로 SET. 게임·장치 상태와 무관하게 물리 상태 저장 |
| 성공 | HTTP 200, `success=true`, `device_name`, 문자열 `life_chip`, `chip_report_ready="1"` |
| 거절 | 비정상 값·대상·등록 불일치는 non-2xx, `success=false`. 기존 증감으로 fallback하지 않음 |
| 펌웨어 확정 | 새 `ReceiveMine`의 같은 장치명·정확한 0/1·`chip_report_ready="1"`을 확인 |
| 재시도 | 동일 값 반복은 누적하지 않음. 실패는 최소 5초 후 최신 물리값으로 재시도 |
| 재동기화 | 첫 입력·재접속·재배정·서버 재시작에서 현재 값 보고. 과거 탈착 큐는 재생하지 않음 |
| 서버 재시작 | 현재 프로세스가 보고를 받은 MAC/key만 ready=1. 재시작 후 ready=0으로 돌려 펌웨어 재보고 유도 |
| 역할/카운트 | 역할은 서버가 결정. 펌웨어는 기존 `revival_count` SET만 유지하며 역할 전환 때 이전 카운트 명령 폐기 |

서버는 엔진이 해당 그룹에서 실행 중이고 글러브도 본게임 활성 상태인 경우에만 역할을 판단한다. 준비·탐색·종료에서는 칩 저장만 수행한다. 포획은 이전 칩 1→새 보고 0, `role=player`, 지정된 같은 그룹의 실제 술래가 `role=tagger` 및 `device_state=activate`인 경우다. 준비 중 이미 없던 칩의 0을 게임 시작 후 다시 보내도 새 포획으로 해석하지 않는다. 빈 `tagger_name`을 임의의 술래로 대체하지 않는다. 이전 칩 값은 해당 프로세스에서 마지막으로 받은 물리 샘플을 사용하며, 최초 연결·서버 재시작으로 샘플이 없을 때에는 DB의 `life_chip`을 기준으로 삼는다. 따라서 DB 1 상태에서 처음 보고한 0도 활성 포획 조건을 만족하면 유령으로 전환한다. 서버 재시작 동안 DB 값을 별도로 변경한 경우와 실제 탈착은 이 계약만으로 구별할 수 없다.

유령이 칩 1을 보고하면 `is_sacrificed=0` 및 `is_open=0`인 봉헌 전 대기에서는 취소 복귀하고, 봉헌 후에는 `revival_count=4` 및 `is_open=1`일 때만 생존자로 복귀한다. 서버가 카운트·개방·봉헌을 갱신한 뒤에도 보고된 칩 상태로 조건을 재검사한다. 새 API를 사용한 등록 장치에만 이 판정을 적용하며, 기존 증감/역할 보고 펌웨어의 동작은 유지한다. 실제 역할 변경이 저장된 경우에만 역할 기록과 엔진 알림을 보낸다.

활성 서버 `role=player` 동안 칩 제거·재장착에도 초록 4칸을 유지한다. 서버에서 `role=ghost`를 확인한 후 유령 표시·상태 진동을 적용한다. setting/ready의 3/4칸 표시와 독립 훈련의 칩 제거 즉시 파랑·9초 소생은 별도 기존 규칙이다.

**적용 순서:** 서버 변경 배포 → 등록 장치 API 확인 → 새 글러브 펌웨어 Release/OTA. 저장소 코드 변경이나 컴파일은 운영 서버 적용을 뜻하지 않는다. 이번 범위는 물리 칩 보고와 글러브 역할 판정이며, 아래에 남긴 전역 생명 추적·제단 봉헌의 모든 기획 차이를 해결한 것은 아니다.

## 기존 API 조사 기록 — 2026-09-16

아래는 신규 절대값 API 도입 전 조사 기록이다. 증감·역할 직접 전송에 관한 설명은 구형 경로의 계약이며, 새 글러브 구현은 위 확정 계약을 따른다.

조사일: 2026-09-16. 기준: [fuzzyline-core main `bc6907f`](https://github.com/Fuzzyline-HAS2/fuzzyline-core/tree/bc6907fa78c9cae713bbbf068d0ad766fc12a92b). 소스와 기존 테스트 내용을 읽은 결과이며 운영 서버/DB에 접속하거나 서버를 실행하지 않았다. 서버 코드도 수정하지 않았다.

### 당시 API 계약

Node의 `GET /api/esp`와 호환 별칭 `GET /has2.php`를 사용한다. 서버는 기본 5000번과 선택적 ESP 호환 8080번 리스너를 제공한다. 펌웨어가 사용할 실제 호스트/포트는 `HAS2_Wifi first_store` 및 현장 설정을 맞춰야 한다. `/has2.php`가 있다고 Apache/PHP 서버로 가정하지 않는다. [README](https://github.com/Fuzzyline-HAS2/fuzzyline-core/blob/bc6907fa78c9cae713bbbf068d0ad766fc12a92b/README.md)

| 작업 | 요청 내용 | 의미와 주의 |
| --- | --- | --- |
| 자기 장치 조회 | `request=ReceiveMine&table=device&mac=<MAC>` | 등록 MAC으로 장치와 그룹 상태를 조회한다. 미등록이면 HTTP 200 + `{}`도 가능 |
| 변경 폴링 | `request=Loop&table=device&mac=<MAC>` | `shift_machine`, watchdog 등 확인. 최초/재접속 시 전체 동기화 유도 |
| 상태/측정 보고 | `request=Send&table=device&key=<자기 장치>&column=<필드>&value=<값>` | 필드마다 SET/증감/별도 로직이 달라 일괄 취급 금지 |
| 다른 장치 조회 | `request=Receive&table=device&key=<대상>` | 역할 등 조회. 요청 대상과 응답을 검증 |
| 장치 사건 | `request=Situation&table=<사건>&key=<주체>&value=<대상>` | 사건별 주체/대상이 다르며 HTTP 200이 게임 처리 성공을 뜻하지 않음 |

본게임 `device_type=iotglove`, `G1/G2`는 `iotglove_g1/g2`로 해석된다. 훈련소는 `training_device`/`training_iotglove`로 별도 해석한다. `ReceiveMine`의 필드는 숫자도 문자열로 반환되며 null은 null이다. 응답 전 `shift_machine=0`으로 낮추므로 응답 유실 때도 flag만 믿지 말고 주기적인 전체 재조회로 복구한다. [타입/테이블 처리](https://github.com/Fuzzyline-HAS2/fuzzyline-core/blob/bc6907fa78c9cae713bbbf068d0ad766fc12a92b/store/web-server/esp-routes.js#L368), [ReceiveMine](https://github.com/Fuzzyline-HAS2/fuzzyline-core/blob/bc6907fa78c9cae713bbbf068d0ad766fc12a92b/store/web-server/esp-routes.js#L2099)

### 당시 글러브 필드와 시간

| 필드 | 현재 서버 동작 | 펌웨어 계획 |
| --- | --- | --- |
| `role` | Origin은 `neutral/player/tagger/ghost`; `revival` 입력도 `ghost`로 정규화 | 다른 매장처럼 `revival`을 유령 wire 값으로 고정하지 않음 |
| `life_chip` | **현재값에 value를 더함** | 칩 유무 스냅샷 0/1을 반복 전송하지 않음. 증감 보고의 중복·응답 유실을 별도 해결 |
| `revival_count` | 정수 SET | 글러브가 시간 경과에 따라 0~4를 보고. 발각 reset도 값 0 보고를 기본안으로 검토 |
| `revival_time` | Origin에서는 **카운트 한 칸의 충전 시간(초)** | count=0에서 시작하면 총 `4 × revival_time`. 60이면 240초이며 훈련소 9초와 다름. 실제 운영 값은 DB 조회 전 미확인 |
| `is_open` | 정수 SET | 생명장치를 이미 열었는지 나타냄 |
| `is_sacrificed` | 정수 SET | 생명칩 봉헌 여부 필드. 존재 자체가 자동 포획 추적 구현을 뜻하지 않음 |
| `battery_remaining` | 실수 SET; DB REAL | 전압 또는 %로 변환하지 않고 받은 실수를 저장 |
| `location` | 문자열 SET, 서버가 `vibe` 재계산 | Origin 방 ID를 사용 |
| `vibe` | 위치 계산 결과 수신 | 같은 방 **3**, 인접 방 **1**, 그 외 **0**. 주석의 같은 방=2보다 실제 함수 반환값을 기준으로 함 |
| `esp_version` | 공통 device 메타에 저장 | TTGO 버전 보고. Nextion 버전은 신규 펌웨어에서 보고하지 않음 |

근거: [SET/증감 분류](https://github.com/Fuzzyline-HAS2/fuzzyline-core/blob/bc6907fa78c9cae713bbbf068d0ad766fc12a92b/store/web-server/esp-routes.js#L204), [숫자 처리](https://github.com/Fuzzyline-HAS2/fuzzyline-core/blob/bc6907fa78c9cae713bbbf068d0ad766fc12a92b/store/web-server/esp-routes.js#L3288), [카운트 소유권/시간 해석](https://github.com/Fuzzyline-HAS2/fuzzyline-core/blob/bc6907fa78c9cae713bbbf068d0ad766fc12a92b/store/web-server/lib/glove-role-timer.js#L134), [테마별 역할/타이머](https://github.com/Fuzzyline-HAS2/fuzzyline-core/blob/bc6907fa78c9cae713bbbf068d0ad766fc12a92b/store/web-server/theme-family.js#L57).

`role=ghost` 보고는 서버에서 `is_open=0`, `is_sacrificed=0`을 함께 설정한다. 따라서 역할 보고를 재시도 가능한 단순 SET으로 취급하면 이미 진행된 상태를 지울 수 있다. `role=player` 보고는 life_chip을 최소 1로 만들며, Origin에서는 Error 전용 `revival_due_at` 검사를 적용하지 않는다. 본게임 setting/stop 중 역할 보고는 성공 ACK를 반환해도 무시될 수 있다. [역할 처리](https://github.com/Fuzzyline-HAS2/fuzzyline-core/blob/bc6907fa78c9cae713bbbf068d0ad766fc12a92b/store/web-server/esp-routes.js#L1534), [Send 역할 분기](https://github.com/Fuzzyline-HAS2/fuzzyline-core/blob/bc6907fa78c9cae713bbbf068d0ad766fc12a92b/store/web-server/esp-routes.js#L2640)

Origin에서는 컨택 단축 `Situation/revival_cooldown`이 무시된다. Error의 서버 만료시각 방식과 Origin의 펌웨어 충전 카운트 방식을 섞지 않는다. count 증가, 시작 시점, 발각 reset, 운영자가 count/time을 바꿀 때의 로컬 기준점 복구를 별도로 설계한다. [컨택 처리](https://github.com/Fuzzyline-HAS2/fuzzyline-core/blob/bc6907fa78c9cae713bbbf068d0ad766fc12a92b/store/web-server/esp-routes.js#L4438)

### 당시 Notion 상세 규칙과 확인된 차이

다음은 확인한 main의 동작이며, 운영 브랜치나 추가 구현이 있는지 확인해야 한다. 기획을 삭제하거나 서버가 이미 전부 처리한다고 가정하지 않는다.

1. **유령 대기/봉헌 확정:** 상태를 표현할 `ghost`, `is_sacrificed`, `is_open`은 있다. UI도 봉헌 전 ghost를 구분한다. 다만 조사한 서버 소스에서는 제단 봉헌 대상을 확정해 `is_sacrificed`를 올리거나 “미확정 생명 수 = 문제 유령 수”로 일괄 복구하는 자동 처리 경로를 찾지 못했다. 제단 봉헌 큐는 생명장치의 활성화 시점을 결정하는 별도 기능이다. [UI](https://github.com/Fuzzyline-HAS2/fuzzyline-core/blob/bc6907fa78c9cae713bbbf068d0ad766fc12a92b/store/web-client/src/pages/component_dashboard/PlayerCard.js#L285), [제단→활성화 큐](https://github.com/Fuzzyline-HAS2/fuzzyline-core/blob/bc6907fa78c9cae713bbbf068d0ad766fc12a92b/store/web-server/game-engine.js#L4417)
2. **생명장치 개방:** `Situation/revival_machine`은 `key=생명장치`, `value=글러브`다. 현재 서버는 장치 상태와 `role=ghost`를 검사하지만, 이 분기에서 글러브의 `revival_count=4`, `is_sacrificed=1`, `is_open=0`을 모두 검사하지 않는다. `is_open=1` 보고는 New_HAS1 생명장치 펌웨어의 개방 처리에 있다. 따라서 글러브 추가만으로 서버 개방 조건까지 완성되지 않는다. [서버 검사](https://github.com/Fuzzyline-HAS2/fuzzyline-core/blob/bc6907fa78c9cae713bbbf068d0ad766fc12a92b/store/web-server/esp-routes.js#L4101), [현재 생명장치 펌웨어](../../HAS1_revival_machine/sensor.ino)
3. **칩 이벤트:** `life_chip`은 증감이고, 전용 물리 칩 장착/제거 및 발각 요청 타입은 조사한 허용 목록에 없다. 단순히 새 사건명을 만들어 호출할 수 없다. `life_chip`, `role`, `revival_count`를 순서대로 쓰는 방식을 쓰더라도 중간 상태·역할 재전송·응답 유실 복구가 필요하다. 최신 기획의 전역 정합성은 서버와 계약을 확정해야 한다.
4. **훈련소:** 서버의 G9P1 초기 역할은 tagger, G9P3~9는 player, G9P2는 동적 역할(신규 시드 neutral)이다. Origin의 G9P2 `is_open=0/is_sacrificed=1/revival_count=4`는 고정 보호된다. 이 서버용 태그 권한을 독립 훈련소 샘플의 실제 LED 9초 충전과 동일 상태로 취급하지 않는다. [훈련 계약](https://github.com/Fuzzyline-HAS2/fuzzyline-core/blob/bc6907fa78c9cae713bbbf068d0ad766fc12a92b/store/web-server/lib/training-glove-roles.js)

개념 상태 매핑의 후보는 `player`=생존자, `ghost+is_sacrificed=0`=유령 대기, `ghost+is_sacrificed=1+is_open=0`=확정 유령, `ghost+is_open=1`=부활 직전이다. 이는 현 필드로 기획을 표현하는 **설계안**이며 모든 자동 전이가 구현되었다는 뜻은 아니다. 칩 플래그는 별도로 유지한다.

### 당시 위치/배터리

- Origin 방 ID와 인접 관계는 [config/audio-layouts.json](https://github.com/Fuzzyline-HAS2/fuzzyline-core/blob/bc6907fa78c9cae713bbbf068d0ad766fc12a92b/config/audio-layouts.json)의 origin을 따른다. 주요 방은 `bambooForest/livingRoom/sleepingRoom/toilet/undergroundRoom/hallway`. 비콘 장치명→방 매핑은 실제 장치 메타와 대조한다.
- `computeVibe`는 실제로 같은 방=3을 반환한다. 위치가 비어 있으면 0이지만 같은 미확인 문자열 두 개를 같은 방으로 볼 가능성이 있으므로, 펌웨어의 위치 유효성/만료 조건을 진동에 적용하고 서버의 unknown 처리도 점검한다. [계산 함수](https://github.com/Fuzzyline-HAS2/fuzzyline-core/blob/bc6907fa78c9cae713bbbf068d0ad766fc12a92b/store/web-server/esp-routes.js#L160)
- 구 글러브는 `battery_remaining`에 소수 2자리 **전압(V)**을 보낸다. 서버는 실수 저장만 하며, 별도 [칼럼 설명 문서](https://github.com/Fuzzyline-HAS2/fuzzyline-core/blob/bc6907fa78c9cae713bbbf068d0ad766fc12a92b/docs/iot-glove-table-columns.md)는 %로 설명해 불일치한다. 기본안은 기존 전압 보고 호환이며, 단위를 명시하고 화면/운영값을 확인한 뒤 적용한다. 임의로 0~100 값을 혼용하지 않는다.
- 하드웨어 배터리 필드는 게임 아이템 수량 `battery_pack`과 별개다. 배터리 미연결·포화 등 무효 측정값을 정상 전압으로 보고하지 않는다.

### 당시 HAS2_Wifi adapter에 필요한 검증

New_HAS1 `b43a316`의 로컬 first_store 사본 기준이다. CI가 가져오는 upstream first_store 최신과 구현 착수 시 다시 대조한다.

- `Send`는 void, `Situation`은 HTTP 200 여부만 반환한다. 특히 `revival_machine`은 성공/거절 모두 HTTP 200에 `open/not_open`을 반환한다. 사업 로직 결과나 확정 상태는 응답/재조회로 확인해야 한다.
- `ReceiveMine/Loop`도 void이고 JSON 파싱 실패를 외부에 반환하지 않는다. 검증된 상태 스냅샷을 만들려면 first_store 라이브러리에 결과/파싱 유효성을 노출하는 보완이 필요한지 확인한다. `shift_machine`과 본문만 보고 최신 성공이라고 단정하지 않는다.
- `MaintainWifi`/`Setup`은 재접속 실패 때 TTGO를 재부팅한다. 네트워크 task를 분리하는 것만으로 단절 중 입력/표시 유지가 보장되지 않는다. first_store에서 재시도/재부팅 정책을 설정 가능하게 할지 검토한다.
- `SendAsync`를 승인 필요한 게임 상태 변경에 사용하지 않는다. 공유 JSON/HTTP는 단일 task가 소유하고, 검증된 복사본을 게임 task로 넘긴다.
- `life_chip` 증감이나 역할 전환의 응답이 사라졌을 때 무조건 재전송하지 않는다. 현재 증감 API에는 요청별 dedup key가 없으므로 재조회 및 재동기화 절차를 계약에 포함한다.

근거: [로컬 라이브러리](../../../libraries/HAS2_Wifi/HAS2_Wifi.cpp). 라이브러리/서버 변경은 이번 문서 조사에서 수행하지 않았으며, 필요 범위와 적용 위치를 구현 전에 확정한다.

### 당시 남은 확인

1. 현재 운영 서버도 조사한 main 기준인지, 최신 Notion 생명 추적을 처리하는 별도 브랜치/코드가 있는지.
2. 배터리 종류/셀 수, TTGO 실물의 ADC 연결과 분압 회로. 기존 GPIO35/raw 보정 상수를 그대로 사용할 근거는 아직 없음.
3. 타이머 시작 시점과 부분 진행 복구, 동적 시간 변경 시 동작, 세부 LED/진동 패턴은 펌웨어 정책으로 정한다. 카운트 한 칸의 단위와 Origin의 비컨택 방식은 소스로 확인 완료.

## 2026-09-18 사용자 확정: 장치 상태와 출력

- `role=tagger` + `device_state=blink`: 술래 결정 전 보라 점멸. `activate`: 보라 상시 점등. 포획 허용은 서버에서 실제 술래의 activate 조건을 검사한다.
- `device_state=setting/ready`: 기존 하양/빨강 색상에서 실제 칩 장착이면 4칸, 미장착이면 3칸. 준비 중에도 칩 상태 0/1을 보고하되 포획·역할 변경은 수행하지 않는다.
- 종료·탐색·무효/만료 응답의 기존 제한을 유지한다. 장치가 setting/ready 표시를 하더라도 원래 game_state가 activate이면 OTA 및 서버 watchdog 리셋의 게임 중 제한을 해제하지 않는다. 수동 `b` 리셋 명령은 기존 동작을 유지한다.
- 상태별 진동 패턴은 펌웨어에 설정한다. 새로운 서버 필드나 대시보드 동작을 요구하지 않는다.
