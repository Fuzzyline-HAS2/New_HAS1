# 발전기 게이지 기여도 기록 설계

작성일: 2026-09-20
대상: `devices/HAS1_generator` (기준 main = `a9228ad`, FIRMWARE_VER 18)
근거: 노션 "누가 얼만큼 전원공급장치 게이지 채웠는지 기록" (본문 없음, 제목만)
상위 항목: "좀 어려운 기타 장치 폴리싱" (중요도 후순위)

## 배경

발전기 수리는 두 단계다.

1. **배선 충전** — 물리 배선 4개를 꽂아 `battery_pack`을 채운다. RFID를 쓰지 않으므로 누가 꽂았는지 알 방법이 없다.
2. **스타터** — 플레이어 카드를 리더 위에 올려둔 채 손잡이(엔코더)를 돌려 GAUGE 28칸을 채운다.
   카드를 떼거나 `role != "player"`면 `EncoderDetach()`로 카운팅이 멈춘다.

"누가 얼만큼 채웠는지"에 답할 수 있는 건 2단계뿐이다. 카드가 올라가 있어야만 게이지가 오르는 구조라
진행량과 사람이 1:1로 묶인다. 이 설계는 스타터 단계만 다룬다.

[`Game_system.ino:26`](../../../devices/HAS1_generator/Game_system.ino#L26)이 이미 새 태그마다
`tagUser`("GxPx" 4바이트)를 읽지만 지역 변수라 `role` 판정 직후 버려진다. 이 값을 세션 동안 들고 있는 것이 변경의 핵심이다.

## 범위

**포함:** 스타터 단계에서 플레이어별 게이지 순증을 서버 이벤트 로그로 전송.

**비포함:**
- 배선 충전 단계 기여자 추적 (RFID 하드웨어가 관여하지 않아 현재 구조로는 불가능)
- 서버 PHP 핸들러 구현 (이 저장소에 서버 코드가 없음 — 계약만 문서화해 넘긴다)
- 결과 화면/집계 UI
- 노션 형제 항목 "통신 구조 바꾸기. mqtt?"

## 세션 정의

**세션** = 카드가 리더에 올라와 `role == "player"`로 확인된 순간부터, 떼어지거나 스타터 단계를 벗어날 때까지.

한 세션이 서버 이벤트 로그 한 행이 된다. 한 플레이어가 여러 번 올렸다 떼면 행도 여러 개이고, 서버가 합산한다.

## 칸 수 기준

`encoderValue / starterEncoderUnit` (28에서 클램프) — `StarterActivate()`가 이미 `gaugeNeoCnt`로 계산하는 목표값.

화면용 `displayedGaugeNeoCnt`를 쓰지 않는 이유: 칸당 최대 100ms 애니메이션 지연이 있어, 빠르게 돌리고 곧바로 카드를 떼면
아직 표시되지 않은 칸이 누락된다. 세션의 시작값과 끝값 모두 목표값 기준이라 뺄셈은 자기일관적이며,
방치 감소로 표시가 거꾸로 따라가는 중이어도 어긋나지 않는다.

## 상태

[`HAS1_generator.h`](../../../devices/HAS1_generator/HAS1_generator.h)에 전역 3개를 추가한다.

| 전역 | 뜻 |
|---|---|
| `String starterContribUser` | 세션 중인 플레이어 코드. 빈 문자열이면 세션 없음 |
| `int starterContribStartCnt` | 세션 시작 시점의 칸 수 |
| `int starterContribLastCnt` | `StarterActivate()`가 매 프레임 갱신하는 최신 칸 수. 스타터를 벗어난 뒤에도 마지막 값이 남는다 |

`StarterActivate()`의 함수-지역 `static bool lastTagState`
([`Game_system.ino:16`](../../../devices/HAS1_generator/Game_system.ino#L16))를 전역으로 올린다.
강제 종료 뒤 리셋해야 카드가 리더에 그대로 얹혀 있어도 복귀 시 새 세션이 열린다.

## 함수

새 탭 `contrib.ino`에 3개.

**`ContribBegin(user, cnt)`** — `starterContribUser`와 `starterContribStartCnt`를 저장한다.

**`ContribEnd(cnt)`** — `delta = cnt - starterContribStartCnt`. `delta >= 1`이면
`has2wifi.Situation(user + ":" + delta, "generator_gauge")`를 1회 호출한다.
결과와 무관하게 시리얼에 한 줄 남기고(`[Contrib] G1P2 8->20 +12 OK` / `skip`) `starterContribUser`를 비운다.
재시도하지 않는다.

**`ContribLoop()`** — `loop()`에서 매 프레임 호출. 세션이 열려 있는데 `ptrCurrentMode != StarterActivate`면
`ContribEnd(starterContribLastCnt)`를 부르고 `lastTagState = false`로 되돌린다.
tagger 인터럽트, 서버의 `setting`/`ready` 리셋처럼 스타터 밖에서 일어나는 모든 이탈을 한 곳에서 잡는다.

**호출 위치는 `ptrCurrentMode()` 바로 앞이다.** 한 프레임 안에서 모드가 바뀔 수 있는 곳은 둘이다 —
`ptrCurrentMode()` 자신과, `TimerRun()`이 부르는 `has2wifi.Loop(DataChanged)`.
`ptrCurrentMode()` 앞에 두면 N번째 프레임 끝의 `DataChanged`가 바꾼 모드를 N+1 프레임 머리에서,
새 모드 함수가 한 번도 돌기 전에 정리한다. `TimerRun()` 뒤에 두면 폴링 GET 바로 뒤에 이 블로킹 GET이
같은 프레임에 연달아 붙는다.

`SettingFunc()`/`ReadyFunc()`에서 `ContribEnd`를 직접 부르지 않는 이유도 같다 —
그 둘은 `DataChanged` 콜백 안에서 실행되므로 블로킹 `Situation()`을 넣으면
`BatteryFinish()`가 `ptrCurrentMode` 대입으로 피해 둔 재진입을 그대로 재현한다.

## 훅 지점

`StarterActivate()` 안 4곳.

1. 태그가 새로 올라오고 `role == "player"`로 판정된 직후 → `ContribBegin`
2. 태그 감지가 `true → false`로 바뀌고 세션이 열려 있으면 → `ContribEnd(현재 칸)`
3. `gaugeNeoCnt`를 계산하고 28로 클램프한 직후 `starterContribLastCnt`를 갱신 —
   **카드 없음/비플레이어 조기 리턴([`Game_system.ino:79`](../../../devices/HAS1_generator/Game_system.ino#L79))보다 위에 둔다.**
   아래에 두면 가드가 걸릴 때마다 값이 낡고, 강제 종료가 낡은 값을 읽는다.
   이 함수는 가드에 걸려도 호출은 되므로 "매 프레임"과 "매 호출"이 다르다.
4. 게이지 완료(`>= NumPixels[GAUGE]`) → `ContribEnd(NumPixels[GAUGE])`을 먼저, 그다음 기존 `StartFinish()`

## 블로킹 비용

`has2wifi.Situation()`은 블로킹 HTTP GET이다. `ContribEnd`가 불리는 세 순간은 모두 `loop()` 컨텍스트이고
엔코더가 멈춰 있거나(카드 뗀 직후, 스타터 이탈 다음 프레임) 이미 블로킹 오디오가 들어 있는 자리(완료 처리 직전)다.
`DataChanged()` 콜백 안에서는 절대 호출하지 않는다 — `BatteryFinish()`를 `ptrCurrentMode` 대입으로 미루는 것과 같은 이유다.

엔코더 펄스는 PCNT 하드웨어가 세므로 이 정지 동안 유실되지 않는다.

**다만 상한이 없다.** AP가 불안정하면 `http.GET()`이 연결 타임아웃까지 앉아 있을 수 있고,
그 사이 카드를 뗀 플레이어 눈에는 LED가 굳어 보인다. 라이브러리의 `SendAsync`가 이 목적의 함수지만
`request=Send`를 하드코딩하고 있어 Situation을 실어 보낼 수 없다.
라이브러리를 건드리지 않기로 했으므로 이 비용은 감수한다. 현장에서 체감되면 그때 라이브러리 쪽으로 옮긴다.

## 서버 계약

서버 담당에게 넘기는 내용이다.

```
GET has2.php?request=Situation&table=generator_gauge&key=<발전기 device_name>&value=<GxPx>:<delta>
```

- `delta`는 정수 1~28 (게이지 칸). 퍼센트가 필요하면 서버에서 28로 나눈다.
- 콜론은 쿼리 컴포넌트에서 이스케이프 없이 유효하다.
- 한 세션 = 한 행. 같은 (발전기, 플레이어) 조합의 행이 여러 개일 수 있고, 합이 그 사람의 기여다.
- 타임스탬프와 판(게임) 연결은 서버가 insert 시점에 붙인다.
- 응답 바디는 쓰지 않는다. HTTP 200이면 성공으로 본다.
- 기존 `revival_machine` Situation 핸들러의 `table` 분기에 case를 추가하는 형태를 예상한다.

부활기가 이미 [`sensor.ino:299`](../../../devices/HAS1_revival_machine/sensor.ino#L299)에서
`Situation(tagUser, "revival_machine")`으로 같은 API를 쓰고 있어 서버 쪽에도 선례가 있다.

## 엣지 케이스

그대로 받아들이는 동작들이다.

- **교대 진행** — A가 20칸까지 채우고 떼면 방치 감소로 15칸이 되고, B가 이어받아 28을 달성한다.
  A는 +20, B는 +13이 된다. 합이 28을 넘지만 세션별 순증이므로 정상이다.
- **`delta <= 0`** — 카드만 올리고 안 돌렸거나 감소가 더 큰 세션. 행을 만들지 않고 로그만 남긴다.
- **비플레이어 카드** — revival/tagger는 세션이 열리지 않는다. `MMMM` 스태프 카드는 스타터 단계에서
  `role` 조회 결과가 player가 아니므로 마찬가지다.
- **tagger 인터럽트, 서버의 `battery_max` 재전송** — 세션을 끊고 행 1개. 복귀 후 같은 사람이 다시 올리면 행이 또 1개.
- **`setting`/`ready` 리셋, 서버가 `repaired`를 직접 세트** — `ContribLoop`이 마지막 칸 기준으로 행 1개를 만든다.
  `SettingFunc()`의 `encoderValue = 100` 리셋은 이미 행이 나간 뒤라 영향이 없다.
- **완료 시 순서** — 기여 행을 먼저, `repaired` 전송을 나중에. 행 전송이 실패해도 수리 완료는 그대로 진행된다.
- **짧게 뗐다 다시 올림** — `TAG_REMOVE_TIME_MS`(500ms) 디바운스가 떼짐으로 보지 않아 한 세션으로 이어진다.
- **500ms 안에 다른 카드로 교체** — 같은 디바운스 때문에 `lastTagState`가 true로 유지되어 `role`을 다시 조회하지 않는다.
  이전 사람의 판정이 그대로 쓰여 최대 500ms(대략 한 칸) 분량이 엉뚱한 사람에게 붙는다.
  기존 RFID 디바운스에서 오는 선행 동작이고 크기가 제한적이라 그대로 둔다.
- **재부팅/워치독** — 진행 중 세션은 RAM에 있어 유실된다.

**불변식:** `ContribLoop()`이 `ptrCurrentMode()`보다 앞에 있다는 점이 두 가지를 동시에 보장한다.
하나는 위에 적은 모드 전환 정리 순서이고, 다른 하나는 `encoderValue`를 리셋하는 두 곳
(`BatteryFinish()`의 `= 1`, `SettingFunc()`의 `= 100`)보다 세션 종료가 항상 먼저 일어난다는 것이다.
`BatteryFinish()`는 오직 `ptrCurrentMode`로만 실행되고
(`WirePollMain()`이 [`wire.ino:117`](../../../devices/HAS1_generator/wire.ino#L117)에서 직접 부르는 경우도
그 프레임의 `ptrCurrentMode`는 이미 `StarterActivate`가 아니라 세션이 그 프레임 머리에서 닫힌 뒤다),
`SettingFunc()`은 프레임 끝 `DataChanged` 안에서 돈다.
그래서 강제 종료는 언제나 리셋 이전의 칸 수로 확정된다. 이 호출을 `TimerRun()` 뒤로 옮기면 보장이 깨진다.

**알려진 한계:** `SettingFunc()`/`ReadyFunc()`은 `starterContribUser`를 직접 비우지 않는다.
열린 세션의 정리를 전적으로 `ContribLoop`에 맡기는 구조이며, 위 불변식이 지켜지는 한 문제가 없다.
두 함수를 나중에 고칠 때 이 의존을 모르면 귀속이 조용히 틀어질 수 있다.

**알려진 한계:** `ContribLoop`이 세션을 강제 종료할 때 `lastTagState`만 되돌리고
`tagOnReader` / `isPlayerTagged`([`Game_system.ino:15`](../../../devices/HAS1_generator/Game_system.ino#L15))는
건드리지 않는다. 스타터로 복귀한 직후 최대 200ms 동안 이 두 값이 직전 판정을 그대로 들고 있어
엔코더는 도는데 세션은 아직 안 열린 구간이 생긴다. 최대 200ms 분량이라 기록 정확도에 실질적 영향은 없다.

## 검증

**로컬 컴파일** — 이 명령으로 확인한다. 브랜치 `claude/generator-contrib-log`의 기준 커밋에서 통과를 확인했고,
플래시는 1304577 / 1966080 바이트(66%)로 여유가 충분하다.

```bash
arduino-cli compile --fqbn "$(python3 scripts/firmware_targets.py HAS1_generator | grep '^FQBN=' | cut -d= -f2-)" --library "$PWD/libraries/HAS1BleBeacon" --library "$AJ7/ArduinoJson" devices/HAS1_generator
```

`$AJ7`은 ArduinoJson 7.4.3을 풀어둔 아무 디렉터리다
(`curl -sL https://github.com/bblanchon/ArduinoJson/archive/refs/tags/v7.4.3.zip -o aj.zip && unzip aj.zip` 후 `ArduinoJson-7.4.3`을 `ArduinoJson`으로 rename).

`--library`로 7.x를 따로 얹는 이유: 개발 PC의 스케치북에 6.19.4가 깔려 있는데
[`wifi.ino:24`](../../../devices/HAS1_generator/wifi.ino#L24)의 `JsonDocument cur;`는 v7 전용 타입이라
그대로 두면 이 변경과 무관하게 컴파일이 깨진다. 스케치북을 고치면 다른 매장 빌드에 영향이 가므로 손대지 않는다.
CI는 7.4.3을 따로 설치하므로 영향받지 않는다.

로컬의 PN532(1.2.2 vs 1.3.4), BusIO(1.11.6 vs 1.17.4), DFPlayer(1.0.5 vs 1.0.6)는 CI보다 낮다.
로컬 컴파일은 문법 확인용이고, 최종 판정은 CI 컴파일이다.

**실기 텔넷 로그** — 5가지 시나리오.

1. 한 사람이 끝까지 채움 → `[Contrib] X 0->28 +28 OK` 한 줄
2. 두 사람 교대 → 행 2개, 각자 자기 구간
3. 카드만 올리고 안 돌림 → `skip`, 전송 없음
4. 진행 중 tagger 전환 → 행 1개, 복귀 후 다시 돌리면 새 세션
5. 진행 중 `setting` 리셋 → 행 1개

**호스트 테스트 하네스는 만들지 않는다.** 덕트는 오디오 시퀀서라는 시간 의존 로직이 있어 하네스 값이 있었지만,
여기는 로직이 25줄 남짓이고 검증 대상이 대부분 RFID/서버 I/O 경계라 스텁 발판이 대상 코드보다 커진다.

## 배포

브랜치 `claude/generator-contrib-log`(main에서 분기). 덕트 폴리싱 작업(`claude/duct-polish`)과 분리한다.

CI 컴파일 통과 후 배포:

```bash
gh workflow run deploy-firmware.yml --ref claude/generator-contrib-log -f device=HAS1_generator -f update_partition=false
```

배포 전 다른 브랜치가 같은 device를 먼저 배포했는지 `gh run list --workflow deploy-firmware.yml`로 확인한다.
두 브랜치가 각자 main에서 버전을 올리면 같은 `vN`이 두 번 나와 장치가 OTA를 건너뛴다.
실제 OTA 트리거(`device_state=github`)는 사용자가 게임 서버에서 직접 한다.

**서버 핸들러가 없으면** 발전기는 HTTP 404/500을 받고 로그만 남긴 채 게임을 계속 진행한다.
펌웨어 배포와 서버 작업의 순서 제약은 없다.
