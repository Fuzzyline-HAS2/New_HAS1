# 태그 유지 vs 즉시 제거 — 솔레노이드 개방 지연 측정 절차 (v49)

현장 증상: **PN532에 태그를 유지하면 솔레노이드 동작 시작이 느리고, 태그 후 바로 떼면 빠르다.**

기기는 로그에 시각을 찍지 않으므로(`telnet.ino`는 `Serial`을 그대로 미러링할 뿐이다) 받는 쪽에서
줄마다 시각을 붙여야 한다. `capture_telnet.py`가 그 역할을 한다.

## 0. 준비

- 캡처하는 PC가 기기와 같은 Wi-Fi(`172.30.1.0/24`)에 있어야 한다.
- 기기 텔넷 서버는 **동시 접속 1개만** 허용한다(`telnet.ino` `TelnetRun`). PuTTY 등 다른 텔넷
  창이 열려 있으면 `Telnet already connected.`만 받고 끊긴다. 먼저 닫을 것.
- 펌웨어가 v49인지 확인한다. 부팅 로그의 `esp_version` 전송 또는 서버의 해당 컬럼으로 본다.

```bash
python3 devices/HAS1_revival_machine/scripts/capture_telnet.py --out revival_ab.log
```

실행 중 문구를 입력하고 Enter를 치면 그 시점에 `### MARK <문구>` 줄이 삽입된다. 시행 구분에 쓴다.
종료는 `q` + Enter.

## 1. 반드시 지켜야 하는 전제 — 글러브를 돌려쓰면 측정이 무효가 된다

한 번 개방에 성공하면 `DataChange()`가 그 글러브의 `is_open`을 1로 쓴다(`game_state.ino:137`).
`is_open=1`인 글러브로 다시 태그하면 `sensor.ino:233-239`의 게이트에 걸려 보라색 점멸만 하고
**문이 열리지 않는다.** 즉 같은 글러브로 두 번째 시행을 하면 그건 개방 지연 측정이 아니다.

따라서 둘 중 하나를 준비한다.

- **권장**: 유령 글러브를 시행 수만큼(최소 6개) 준비해 매 시행마다 다른 글러브를 쓴다.
- 또는 매 시행 직후 서버에서 그 글러브의 `is_open`을 0으로 되돌린다.

각 시행 전에 반드시 확인할 것:

| 항목 | 값 |
|---|---|
| `game_state` | `activate` |
| 생명장치 `device_state` | `activate` (직전 개방의 5초 통전이 끝나고 서버가 되돌린 뒤) |
| 글러브 `role` | `ghost` |
| 글러브 `is_open` | `0` |

`role`이 유령이 아니면 서버가 `open`을 주지 않는 게 정상 동작이라 측정 자체가 성립하지 않는다.
v49는 이 경우 `[Approval] wait ended: role not eligible`을 찍고 대기를 끝낸다.

## 2. v49에서 시행 사이에 태그를 떼는 시간

v49는 붙여둔 같은 태그를 래치한다(`gameplay_tag_latched`). 래치가 풀리는 조건은
**양쪽 Gain 판독이 2회 이상 실패하고, 그 실패가 600ms(`RFID_REARM_ABSENT_MS`) 이상 이어질 때**다
(`approval.ino` `ObserveGameplayTag`).

그리고 승인 대기 중에는 `RfidLoop()`가 곧바로 `AdminCardPollPending()`으로 빠지므로
(`sensor.ino` v49) **`ObserveGameplayTag`가 아예 호출되지 않는다** — 즉 대기 중에 태그를 떼도
그 시간은 래치 해제에 카운트되지 않는다. 대기가 끝난 뒤부터 600ms를 다시 세야 한다.

**시행 사이에는 태그를 리더에서 완전히 떼고 최소 2초를 둔다.**

## 3. 시행 절차

A(유지) 3회, B(즉시 제거) 3회를 **번갈아** 한다. 몰아서 하면 AP 혼잡도 변화가 A/B 차이로
오인될 수 있다.

각 시행마다:

1. 마커 입력: `A1 hold` (또는 `B1 release`)
2. 글러브를 PN532에 댄다.
3. **A(유지)**: 문이 열리고 5초 통전이 끝날 때까지 계속 붙여둔 뒤, 추가로 5초 더 붙여둔다.
   (개방 이후 `is_open=1` 경로와 유령 재개방 경로를 함께 관찰하기 위함)
   **B(즉시 제거)**: 대는 즉시 — 0.2초 이내 — 뗀다.
4. 솔레노이드 소리가 나면 마커 입력: `A1 relay` (귀로 들은 시각이 로그의 릴레이 시각과 맞는지
   교차 확인용. 정확도는 로그 쪽이 높으니 참고용이다.)
5. 태그를 떼고 2초 이상 기다린다. `device_state`가 `activate`로 돌아온 것을 확인한다.
6. 다음 시행은 **다른 글러브**로.

추가로 다음 두 가지를 각 1회씩 하면 원인 분리에 크게 도움이 된다.

- **C(혼잡 없음)**: 다른 iotGlove의 전원을 모두 끈 상태에서 A와 B를 1회씩.
  9/14 리포트(`3f6b7cd`)의 "단일 기기 0.3~1초 / 여러 대 6~7초"가 이번에도 재현되는지,
  그리고 A/B 차이가 혼잡과 독립인지 갈린다.
- **D(관리자 카드)**: `MMMM` 카드를 유지한 채로 1회. 이 경로는 서버 왕복이 전혀 없으므로
  (`sensor.ino:155-160`) 순수 PN532 + 루프 비용만 남는다. A가 느린 이유가 통신인지
  PN532인지 가르는 대조군이다.

## 4. 캡처를 끝낸 뒤

`q` + Enter로 종료한다. 로그를 편집하거나 잘라내지 말 것 — 줄 사이의 **침묵 구간**이 측정
대상이라 중간을 지우면 분석이 불가능해진다.

```bash
python3 devices/HAS1_revival_machine/scripts/analyze_capture.py revival_ab.log
python3 devices/HAS1_revival_machine/scripts/analyze_capture.py revival_ab.log --timeline   # 전체 타임라인
python3 devices/HAS1_revival_machine/scripts/analyze_capture.py --selftest                  # 파서 자체 검증
```

## 5. 로그에서 무엇이 보이고 무엇이 안 보이는가 (v49 소스 확인 결과)

분석기는 아래 사실 위에 세워져 있다. `HAS2_Wifi::HttpRequest`는 `request`가 `"Loop"`가
**아닐 때만** 응답을 콘솔에 찍는다(`HAS2_Wifi.cpp:656-663`).

| 동작 | 콘솔 출력 | 셀 수 있나 |
|---|---|---|
| 성공한 `request=Loop` 폴링 | **아무것도 없음** | ✗ 침묵 구간으로만 보인다 |
| `Receive` (글러브 role/is_open) | 글러브 행 JSON 전체 | ✓ |
| `ReceiveMine` (기기 행) | 기기 행 JSON 전체 | ✓ |
| 승인 대기 폴링 1회 | 기기 행 JSON + `Data Change` | ✓ **폴링 횟수를 직접 셀 수 있다** |
| `Situation` | 본문 또는 `HTTP GET... code: 200, empty body, request: <URL>` | ✓ `[RFID] Situation send ... took=` 줄로 확정 |
| HTTP 실패 | 요청 URL과 코드/에러 | ✓ |

**가장 중요한 함정**: `[GhostTiming] RELAY ON` 줄은 `SolenoidPulse(5000)`의 5초 블로킹
delay가 **끝난 뒤에** 찍힌다(`game_state.ino:119-131`). 그러므로 이 줄의 호스트 시각은 실제
릴레이 HIGH보다 약 5초 늦다. PR #27이 정확히 이걸 놓쳐 오진했다.

- 줄에 실린 `total` 값은 기기가 `tagDetectedMs` → 실제 GPIO HIGH로 직접 잰 값이라 5초가
  섞여 있지 않다. **이 값이 기준값이다.**
- 분석기는 호스트 시각에서 5000ms를 빼 교차검증하고, 두 값이 400ms 넘게 어긋나면 경고한다.

## 6. 분석기가 내는 판정

`침묵 구간 합계`에서 릴레이 통전 5초는 제외된다. 남은 침묵은 콘솔에 아무것도 남기지 않는 작업,
즉 **PN532 판독/`ApplyGain`, 성공한 `request=Loop` 폴링, `delay()`** 뿐이다.

| 관측 | 결론 |
|---|---|
| 유지 시행에서 `Situation 호출 수 > 1` 또는 `사이클 내 재판독 수 > 0` | v49 래치가 샌다. 반복 판독이 원인 (H1/H2) |
| 래치는 지켜졌는데 유지 쪽 `polls`와 `침묵 구간 합계`가 크다 | 폴링 구조 또는 PN532가 원인 (H3/H4) |
| 유지 쪽 `role 조회`/`Situation` 왕복 자체가 길다 | 기기가 아니라 서버/AP 지연 |
| 유지/제거 차이 200ms 미만 | 재현 실패. 조건(혼잡도·글러브·상태)을 다시 맞춰야 한다 |

`MMMM` 대조군(D)은 서버 왕복이 전혀 없으므로, 여기서도 유지 시 느리면 원인은 통신이 아니라
PN532/루프다.
