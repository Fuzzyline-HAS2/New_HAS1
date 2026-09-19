# HAS1_duct 폴리싱 4건 설계

작성일: 2026-09-19
대상: `devices/HAS1_duct` (현재 main = Firmware v52)
근거: 노션 "기타 장치 폴리싱" 하위 덕트 항목 3건 + 사용자 추가 요청(봉쇄 `left_time`)
비범위: "덕트 EM LOCK 재잠금 시간 늘리기"(미뤄짐)

## 배경

덕트 펌웨어는 DFPlayer 트랙을 `audio_queue.ino`의 시간 기반 시퀀서로 이어 붙여 문장을 만든다.
각 트랙의 길이는 `mp3_durations.h`(WAV 프레임 기준 실측)에서 읽고, 트랙 사이에 `MP3_TRACK_MARGIN_MS` 여유를 둔다.
서버 상태는 2초 간격 폴링(`WifiTimerFunc` → `has2wifi.Loop(DataChange)`)으로 `my[...]`에 들어온다.

## A. 남은 시간 안내 딜레이 축소

**현재:** "해제까지" → (200ms 여유 + DFPlayer 시작 지연) → "N초" → "남았습니다". 현장 체감 약 0.3초, 매번 일정.

**변경:** `audio_queue.h`의 `MP3_TRACK_MARGIN_MS`를 200 → 100으로 낮춘다.

**근거:** 트랙 길이는 WAV 프레임 실측값이라 정확하다. DFPlayer가 재생 명령을 받고 소리를 내기까지 약 100ms가 걸리므로,
여유가 그 이하로 내려가면 다음 명령이 앞 트랙의 끝을 자를 수 있다. 100ms를 안전선으로 잡는다.

**부수 변경:** `tests/audio_harness.cpp`의 경계값(3488/3489, 4747, 2507, `+ 200`)을 새 여유값 기준으로 갱신.
`tests/README.md`, `mp3_durations.h` 머리말의 "200ms" 문구 갱신.

**검증:** 기존 오디오 시나리오 9개가 새 경계값으로 통과. 실기에서 "해제까지" 끝이 잘리지 않는지 사용자가 확인.

## B. 안/밖 개방 음원 분리

**현재:** 개방 경로(RFID 태그, 내부 스위치, 서버 `manage_state=mo`, MMMM 관리자 카드) 모두 01/0002
"덕트 오픈. 손잡이를 강하게 당겨주십시오"를 재생. 안에서 열 때 문구가 맞지 않음.

**변경:**

| 개방 경로 | 한국어 | 영어 |
|---|---|---|
| 내부 스위치(`DuctOpen(true)`) | 09/0719 (1153ms) | 10/0719 (1153ms) |
| 그 외 전부(태그, `mo`, MMMM) | 09/0712 (3289ms) | 10/0712 (2400ms) |

음원 출처: HAS2-Nextion `feat/audio-library-v2` `audios/V2/duct_MP3_22k/09`, `/10` (커밋 e49fa96).
길이는 WAV 프레임을 22050Hz로 나눠 올림한 값(기존 표와 동일 방식).

**언어 매핑:** 현재 `Mp3MakePhrase`는 영어일 때 폴더 +4(01→05 … 04→08). 폴더 09의 영어판은 10이므로
매핑을 함수 하나(`Mp3LanguageFolder(folder, english)`)로 모으고 "9 → 10, 그 외 +4" 규칙을 둔다.
`Mp3PreparePhrase`/`RemainingTimeMp3`가 숫자 트랙(폴더 2/3 → 6/7)에 쓰는 +4는 그대로 둔다.

**코드 위치:**
- `HAS1_duct_function.ino`: `DuctOpen`, `MmmmOpen`의 `Mp3PlayLargeFolder(1, 2)`를 개방 안내 함수 한 곳으로 모아 스위치 여부로 분기.
- `audio_queue.ino`: `Mp3LanguageFolder` 추가, `Mp3MakePhrase`에서 사용.
- `mp3_durations.h`: `case 9`, `case 10`에 712/719 추가. 머리말에 09/10 사용 파일 명시.
- `sensor.ino`의 `Mp3Check()`(호출부 없는 죽은 코드)는 손대지 않는다.

**전제:** 현장 SD카드에 폴더 09, 10과 해당 파일이 있어야 한다. 없으면 개방 안내만 무음이 되고 개방 동작 자체는 영향 없다.

**검증(호스트 테스트):**
- 태그 개방 → `play:9:712`, 스위치 개방 → `play:9:719`, MMMM/`mo` 개방 → `play:9:712`.
- 영어 선택 시 → `play:10:712` / `play:10:719`, 숫자 문장은 기존처럼 `5:3 → 7:N → 5:5`.
- 길이표 픽스처: `Mp3TrackDurationMs(9,712)==3289`, `(9,719)==1153`, `(10,712)==2400`, `(10,719)==1153`.
- 기존 시나리오의 `play:1:2`/`play:5:2` 기대값을 새 트랙으로 갱신.

## C. OS 활성화 시 쿨타임 즉시 해제

**현재:** 서버 `device_state`가 `activate`로 바뀌면 `ExitTaggerMode()`만 호출. 봉쇄가 아닌 일반 쿨타임 중에는 아무 일도 없어
운영 OS의 "활성화" 버튼이 먹지 않는다.

**변경:** `DataChange`의 `activate` 분기가 새 함수 `ServerActivate()`를 호출한다.

```
ServerActivate():
  if (game_state == activate && !duct_available && digitalRead(RELAY_PIN) == LOW)
      CooltimeFinish();        // 쿨타임 즉시 종료
  ExitTaggerMode();            // 기존 봉쇄 해제 동작 유지
```

`CooltimeFinish()`는 `timer.ino`의 `CooltimeTimerFunc` 완료 분기(노란색, `device_state=activate` 전송, `current_time=0`,
`duct_available=true`, `cool_time_neo_bool=false`, 쿨타임 타이머 삭제)를 함수로 뽑아 두 곳에서 공유한다.
`use_duct_num`(사용횟수 사다리)은 건드리지 않는다.

**가드 이유:**
- `game_state != activate` 또는 이미 사용 가능이면 할 일이 없다. 쿨타임이 자연 종료되어 디바이스가 보낸 `activate`가
  다음 폴링에 되돌아올 때도 이 조건으로 무시된다.
- 릴레이 HIGH(문 열림 4초, 관리자 개방 포함) 중에는 무시한다. 이때 상태를 바꾸면 닫힘 콜백(`DuctClose`/`MmmmClose`)이
  쿨타임을 다시 시작해 표시와 실제 상태가 어긋난다. 운영자가 그 4초 안에 누르는 경우는 드물다.
- 봉쇄+쿨타임이 겹친 상태에서는 쿨타임 종료 후 봉쇄 해제 순서로 둘 다 풀린다. `activate` 전송이 두 번 나갈 수 있으나 무해하다.

**알려진 한계:** 서버 DB가 이미 `activate`인데 디바이스는 쿨타임 중인 경우(예: `lock` 전송 실패) 값이 바뀌지 않아 버튼이 감지되지 않는다.
서버 측 변경 없이는 해결할 수 없어 범위 밖으로 둔다.

**검증(호스트 테스트):** `ServerActivate`를 함수 목록에 추가하고
- 쿨타임 중 호출 → 즉시 `duct_available`, 노란색, `activate` 전송, `use_duct_num` 유지.
- 문 열림 중 호출 → 무시, 이후 닫힘에서 정상 쿨타임 시작.
- 봉쇄+쿨타임 중 호출 → 둘 다 해제.
- 사용 가능 상태에서 호출 → 변화 없음.

## D. 봉쇄 남은 시간을 서버 `left_time`으로

**현재:** `tagger_duration_ms = 30000`을 하드코딩하고 `tagger_started_ms`부터 경과로 남은 초를 계산(`TaggerRemainingSeconds`).
헤더 TODO에 "서버 값을 받으면 대체" 명시.

**서버 필드:** 덕트 자기 레코드 `my["left_time"]`, 단위 초, 서버가 카운트다운.

**변경:**
- 전역 추가: `int tagger_left_time_s`, `unsigned long tagger_left_time_ms`(수신 시각), `bool tagger_left_time_valid`.
- `EnterTaggerMode()`에서 `tagger_left_time_valid = false`로 초기화.
- 새 함수 `TaggerLeftTimeUpdate()`: `tagger_mode`일 때 `(int)my["left_time"]`이 0보다 크면 값과 `millis()`를 저장하고 valid로 표시.
  `DataChange`가 매 호출마다 부른다(값이 바뀌지 않아도 최신 수신 시각을 갱신해야 경과 보정이 정확하다).
- `TaggerRemainingSeconds()`: valid면 `left_time_s - (millis() - 수신시각)/1000`(올림, 0 이하는 0). 아니면 기존 30초 계산 유지.
- 봉쇄 해제는 지금처럼 서버 명령(`back`/`activate`)으로만 한다. `left_time`이 0이 되어도 디바이스가 스스로 풀지 않는다.

**0초 처리:** 남은 시간이 0이면 기존 규칙대로 03/0000 파일이 없어 문장 전체가 생략된다(무음). 곧 서버 해제 명령이 오므로 그대로 둔다.

**검증(호스트 테스트):** `TaggerLeftTimeUpdate`, `TaggerRemainingSeconds`로
- `left_time` 없음 → 기본 30초 카운트다운.
- `left_time=25` 수신 후 3초 경과 → 22.
- 재수신 시 최신값 기준으로 갱신.
- 봉쇄 재진입 시 이전 값 무효화.

## 배포

- 브랜치 `claude/duct-polish`에서 작업. 호스트 테스트 통과 후
  `gh workflow run deploy-firmware.yml --ref claude/duct-polish -f device=HAS1_duct -f update_partition=false`.
- 다른 브랜치가 먼저 duct를 배포하지 않았는지 `gh run list --workflow deploy-firmware.yml` 확인(동일 vN 충돌 방지).
- OTA 시작(`device_state=github`)은 사용자가 서버에서 직접 한다.
