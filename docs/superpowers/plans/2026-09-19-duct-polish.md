# HAS1_duct 폴리싱 4건 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 덕트 펌웨어에 (A) 안내 트랙 사이 여유 100ms, (B) 안/밖 개방 음원 분리(폴더 09/10), (C) 서버 `activate`로 쿨타임 즉시 해제, (D) 봉쇄 남은 시간을 서버 `left_time`으로 계산하는 변경을 넣고 호스트 테스트로 검증한다.

**Architecture:** 모든 변경은 `devices/HAS1_duct` 안의 Arduino 스케치(.ino/.h)와 그 호스트 회귀 테스트(`tests/`)에만 닿는다. 테스트 러너 `tests/run_cooldown_tests.py`가 실제 펌웨어 함수를 정규식으로 잘라 `host_harness.cpp`(하드웨어 더블)에 끼워 clang++로 빌드하고 케이스 이름별로 실행한다. 오디오 스케줄러는 `-DACTUAL_AUDIO`로 `audio_queue.ino` 전체를 포함해 `audio_harness.cpp`의 케이스를 돈다.

**Tech Stack:** Arduino(ESP32 TTGO T1), DFRobotDFPlayerMini, SimpleTimer, ArduinoJson(`my[...]`), Python 3 + clang++ 호스트 테스트.

## Global Constraints

- 스펙: `docs/superpowers/specs/2026-09-19-duct-polish-design.md`
- 브랜치: `claude/duct-polish` (이미 생성됨). 커밋은 이 브랜치에만.
- 테스트 명령(저장소 루트에서): `python3 devices/HAS1_duct/tests/run_cooldown_tests.py` — 마지막 두 줄이 `PASS: N firmware cooldown scenarios` / `PASS: M actual audio scheduler scenarios` 여야 한다. 실패 시 `FAIL at <ms> ms: <message>` 한 줄이 나오고 종료코드 1.
- 트랙 여유: `MP3_TRACK_MARGIN_MS = 100`
- 개방 음원: 밖(태그·`manage_state=mo`·MMMM) 09/0712, 안(내부 스위치) 09/0719. 영어는 폴더 10 같은 번호.
- 길이(ms): 09/0712=3289, 09/0719=1153, 10/0712=2400, 10/0719=1153
- 영어 폴더 매핑: 폴더 9 → 10, 그 외 +4
- `left_time`: `my["left_time"]`, 초 단위, 서버 카운트다운. 0 이하·부재는 무시하고 30초 기본값 유지.
- `use_duct_num`은 어느 변경에서도 건드리지 않는다. `sensor.ino`의 `Mp3Check()`(죽은 코드)도 건드리지 않는다.
- 커밋 메시지는 한국어 `fix(duct): …`/`feat(duct): …`/`test(duct): …` 형식. 끝의 attribution 트레일러는 실행 시점에 지정된 것을 쓴다(Task 1~3은 `Claude Fable 5.1`, Task 4부터는 `Claude Opus 5`).
- 테스트 러너의 함수 추출 정규식은 반환형이 `void|int|bool|Mp3Phrase`인 함수만 잡는다. 새 함수는 이 반환형 중 하나로 만든다.

---

## 파일 구조

| 파일 | 역할 | 변경 Task |
|---|---|---|
| `devices/HAS1_duct/audio_queue.h` | 시퀀서 상수·프로토타입 | 1, 2 |
| `devices/HAS1_duct/audio_queue.ino` | 문장 생성/스케줄러. 언어 폴더 매핑 추가 | 2 |
| `devices/HAS1_duct/mp3_durations.h` | 트랙 길이표. 09/10 항목 추가 | 1(주석), 2 |
| `devices/HAS1_duct/HAS1_duct_function.ino` | 덕트 동작. `OpenMp3`, `ServerActivate`, `TaggerLeftTimeUpdate`, `TaggerRemainingSeconds` | 3, 4, 5 |
| `devices/HAS1_duct/timer.ino` | 쿨타임 타이머. 완료 분기를 `CooltimeFinish`로 추출 | 4 |
| `devices/HAS1_duct/game_state.ino` | 서버 폴링 반영(`DataChange`), 봉쇄 진입 | 4, 5 |
| `devices/HAS1_duct/HAS1_duct.h` | 전역·프로토타입 | 3, 4, 5 |
| `devices/HAS1_duct/tests/audio_harness.cpp` | 오디오 스케줄러 케이스 | 1, 2 |
| `devices/HAS1_duct/tests/host_harness.cpp` | 펌웨어 시나리오 케이스 | 3, 4, 5 |
| `devices/HAS1_duct/tests/run_cooldown_tests.py` | 러너: 포함 함수·케이스 목록 | 2, 3, 4, 5 |
| `devices/HAS1_duct/tests/README.md` | 테스트 문서 | 1, 5 |

---

### Task 1: 트랙 사이 여유 200ms → 100ms (스펙 A)

**Files:**
- Modify: `devices/HAS1_duct/audio_queue.h` (`MP3_TRACK_MARGIN_MS`)
- Modify: `devices/HAS1_duct/mp3_durations.h` (머리말 주석)
- Modify: `devices/HAS1_duct/tests/audio_harness.cpp` (경계값)
- Modify: `devices/HAS1_duct/tests/README.md`

**Interfaces:**
- Consumes: 없음
- Produces: `MP3_TRACK_MARGIN_MS == 100` (이후 Task의 시간 계산 기준)

- [ ] **Step 1: 오디오 테스트 경계값을 100ms 기준으로 바꾼다 (실패하는 테스트)**

`devices/HAS1_duct/tests/audio_harness.cpp`에서 아래 문자열을 정확히 치환한다. 각 값은 "트랙 길이 + 여유 − 1"(아직 안 넘어감) / "트랙 길이 + 여유"(넘어감)이다.

| 기존 | 변경 | 위치(케이스) |
|---|---|---|
| `now = 3488; Mp3Run(); check(audioEvents.size() == 1, "opening duration plus margin has not elapsed");` | `now = 3388; Mp3Run(); check(audioEvents.size() == 1, "opening duration plus margin has not elapsed");` | audio_fifo |
| `>= Mp3TrackDurationMs(folders[i], files[i]) + 200,` | `>= Mp3TrackDurationMs(folders[i], files[i]) + MP3_TRACK_MARGIN_MS,` | audio_fifo |
| `advance(3488); check(relay == HIGH && audioEvents.size() == 1, "opening completes before the next track");` | `advance(3388); check(relay == HIGH && audioEvents.size() == 1, "opening completes before the next track");` | audio_door_timers |
| `"queued announcement begins at 3489ms without closing door early"` | `"queued announcement begins at 3389ms without closing door early"` | audio_door_timers |
| `advance(510); check(relay == HIGH, "door remains open until its close deadline");` | `advance(610); check(relay == HIGH, "door remains open until its close deadline");` | audio_door_timers |
| `now = 4747; Mp3Run(); check(audioEvents == std::vector<String>{"play:4:1"},` | `now = 4647; Mp3Run(); check(audioEvents == std::vector<String>{"play:4:1"},` | audio_v2_blockade |
| `now += 2507; Mp3Run(); check(audioEvents.size() == 2, "V2 blockade intro retains its full duration and margin");` | `now += 2407; Mp3Run(); check(audioEvents.size() == 2, "V2 blockade intro retains its full duration and margin");` | audio_v2_blockade |
| `now += 3488; Mp3Run(); check(audioEvents.size() == 1, "millis rollover cannot prematurely finish track");` | `now += 3388; Mp3Run(); check(audioEvents.size() == 1, "millis rollover cannot prematurely finish track");` | audio_wrap |

sed 한 번에 적용:

```bash
cd /Users/byeongjun/workspace/New_HAS1/devices/HAS1_duct/tests && sed -i '' \
  -e 's/now = 3488; Mp3Run();/now = 3388; Mp3Run();/' \
  -e 's/files\[i\]) + 200,/files[i]) + MP3_TRACK_MARGIN_MS,/' \
  -e 's/advance(3488); check(relay == HIGH/advance(3388); check(relay == HIGH/' \
  -e 's/begins at 3489ms without/begins at 3389ms without/' \
  -e 's/advance(510); check(relay == HIGH, "door remains open/advance(610); check(relay == HIGH, "door remains open/' \
  -e 's/now = 4747; Mp3Run();/now = 4647; Mp3Run();/' \
  -e 's/now += 2507; Mp3Run();/now += 2407; Mp3Run();/' \
  -e 's/now += 3488; Mp3Run();/now += 3388; Mp3Run();/' \
  audio_harness.cpp && grep -c '3388\|3389\|4647\|2407\|advance(610)' audio_harness.cpp
```

Expected: 마지막 grep 출력 `7`(치환된 줄 수). `grep -n '3488\|3489\|4747\|2507\|+ 200' audio_harness.cpp`는 아무것도 출력하지 않아야 한다.

- [ ] **Step 2: 테스트가 실패하는지 확인**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && python3 devices/HAS1_duct/tests/run_cooldown_tests.py 2>&1 | tail -3
```

Expected: 펌웨어 시나리오 27개는 개별 `PASS <case>` 줄을 찍고 지나가며, 오디오 첫 케이스에서 `FAIL at 3389 ms: cooldown starts after full opening duration` 뒤 `CalledProcessError`로 끝난다. (요약 `PASS: N …` 두 줄은 전부 통과했을 때만 마지막에 출력된다.)

- [ ] **Step 3: 여유 상수를 100으로 변경**

`devices/HAS1_duct/audio_queue.h`:

```cpp
// 기존
const unsigned long MP3_TRACK_MARGIN_MS = 200;
// 변경
const unsigned long MP3_TRACK_MARGIN_MS = 100;
```

`devices/HAS1_duct/mp3_durations.h` 머리말:

```cpp
// 기존
// 재생 여유 200ms는 시퀀서에서 더한다. 음원 교체 시 길이표도 다시 측정한다.
// 변경
// 재생 여유(MP3_TRACK_MARGIN_MS)는 시퀀서에서 더한다. 음원 교체 시 길이표도 다시 측정한다.
```

`devices/HAS1_duct/tests/README.md`:

```
기존: The scheduler adds its existing 200ms margin separately.
변경: The scheduler adds its `MP3_TRACK_MARGIN_MS` (100ms) margin separately.
```

- [ ] **Step 4: 테스트 통과 확인**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && python3 devices/HAS1_duct/tests/run_cooldown_tests.py 2>&1 | tail -2
```

Expected:
```
PASS: 27 firmware cooldown scenarios
PASS: 9 actual audio scheduler scenarios
```

- [ ] **Step 5: 커밋**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && git add devices/HAS1_duct/audio_queue.h devices/HAS1_duct/mp3_durations.h devices/HAS1_duct/tests/audio_harness.cpp devices/HAS1_duct/tests/README.md && git commit -m "fix(duct): 안내 트랙 사이 여유 200ms → 100ms로 축소

남은 시간 안내에서 \"해제까지\"와 숫자 사이 공백을 줄인다. 트랙 길이는 WAV
실측값이고 DFPlayer 시작 지연이 약 100ms라 그 이하로는 앞 트랙이 잘릴 수 있어
100ms를 안전선으로 둔다. 테스트 경계값을 새 여유 기준으로 갱신.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: 폴더 09/10 길이표 + 영어 폴더 매핑 (스펙 B 기반)

**Files:**
- Modify: `devices/HAS1_duct/mp3_durations.h` (`case 9`, `case 10` 추가, 머리말)
- Modify: `devices/HAS1_duct/audio_queue.h` (프로토타입)
- Modify: `devices/HAS1_duct/audio_queue.ino` (`Mp3LanguageFolder`, `Mp3MakePhrase`)
- Modify: `devices/HAS1_duct/tests/run_cooldown_tests.py` (비-ACTUAL 빌드에 `Mp3LanguageFolder` 포함, 오디오 케이스 추가)
- Test: `devices/HAS1_duct/tests/audio_harness.cpp` (`audio_folder9_language`)

**Interfaces:**
- Consumes: `MP3_TRACK_MARGIN_MS`(Task 1)
- Produces: `int Mp3LanguageFolder(uint8_t folder, bool english)` — 영어면 폴더 9 → 10, 그 외 +4; 한국어면 그대로. `Mp3TrackDurationMs(9|10, 712|719)`가 0이 아닌 값을 돌려준다.

- [ ] **Step 1: 실패하는 오디오 테스트 추가**

`devices/HAS1_duct/tests/audio_harness.cpp`의 `} else if (test == "audio_stale") {` 블록이 끝나는 곳, 즉 `} else return 2;` 바로 앞에 삽입:

```cpp
    } else if (test == "audio_folder9_language") {
        check(Mp3TrackDurationMs(9, 712) == 3289 && Mp3TrackDurationMs(9, 719) == 1153,
              "measured V2 folder 09 Korean opening fixtures");
        check(Mp3TrackDurationMs(10, 712) == 2400 && Mp3TrackDurationMs(10, 719) == 1153,
              "measured V2 folder 10 English opening fixtures");
        Mp3PlayLargeFolder(9, 719); drainAudio();
        check(audioEvents == std::vector<String>{"play:9:719"}, "Korean keeps folder 09");
        shift_machine["selected_language"] = "EN";
        Mp3PlayLargeFolder(9, 712); Mp3PlayLargeFolder(9, 719); Mp3PlayLargeFolder(1, 2); drainAudio();
        check(audioEvents == std::vector<String>{"play:9:719", "play:10:712", "play:10:719", "play:5:2"},
              "English maps folder 09 to 10 and other folders by +4");
        check(audioStartTimes[2] - audioStartTimes[1] == 2400 + MP3_TRACK_MARGIN_MS,
              "English outside opening uses its own measured length");
```

`devices/HAS1_duct/tests/run_cooldown_tests.py`의 `audio_cases` 목록 끝에 `"audio_folder9_language"` 추가:

```python
    audio_cases = ["audio_fifo", "audio_door_timers", "audio_v2_blockade", "audio_overflow", "audio_duplicate",
                   "audio_missing", "audio_wrap", "audio_language", "audio_stale", "audio_folder9_language"]
```

- [ ] **Step 2: 실패 확인**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && python3 devices/HAS1_duct/tests/run_cooldown_tests.py 2>&1 | grep -E 'FAIL|PASS:' | head -3
```

Expected: `FAIL at 0 ms: measured V2 folder 09 Korean opening fixtures`

- [ ] **Step 3: 길이표에 09/10 추가**

`devices/HAS1_duct/mp3_durations.h`에서 `case 8:` 블록 뒤, `default: return 0;` 앞에 삽입:

```cpp
    case 9:
        switch (track) {
        case 712: return 3289UL;
        case 719: return 1153UL;
        default: return 0;
        }
    case 10:
        switch (track) {
        case 712: return 2400UL;
        case 719: return 1153UL;
        default: return 0;
        }
```

머리말의 Path 줄을 바꾼다:

```cpp
// 기존
// Path: audios/V2/duct_MP3_22k/01~08 (154 WAV files).
// 변경
// Path: audios/V2/duct_MP3_22k/01~08 (154 WAV files)
//       + 09/0712, 09/0719 (한국어 개방 안내), 10/0712, 10/0719 (영어 개방 안내).
```

- [ ] **Step 4: 언어 폴더 매핑 함수 추가**

`devices/HAS1_duct/audio_queue.h`의 `Mp3Phrase Mp3MakePhrase(uint8_t folder, uint16_t file);` 위에 추가:

```cpp
int Mp3LanguageFolder(uint8_t folder, bool english);
```

`devices/HAS1_duct/audio_queue.ino`의 `Mp3MakePhrase` 를 아래로 교체(함수 앞에 `Mp3LanguageFolder` 정의):

```cpp
// 영어 음원 폴더: 01~04 → 05~08(+4). 폴더 09(개방 안내 합성음)는 영어판이 10이다.
int Mp3LanguageFolder(uint8_t folder, bool english)
{
    if (!english) return folder;
    return folder == 9 ? 10 : folder + 4;
}

Mp3Phrase Mp3MakePhrase(uint8_t folder, uint16_t file)
{
    Mp3Phrase phrase = {};
    bool english = (String)(const char *)shift_machine["selected_language"] == "EN";
    phrase.count = 1;
    phrase.volume = english ? 26 : 30;
    phrase.tracks[0].folder = (uint8_t)Mp3LanguageFolder(folder, english);
    phrase.tracks[0].file = file;
    return phrase;
}
```

`devices/HAS1_duct/tests/run_cooldown_tests.py`에서 비-ACTUAL 빌드용 팩토리에 새 함수를 함께 넣는다:

```python
# 기존
          .replace("// DOMAIN_AUDIO_FACTORY", function(audio, "Mp3MakePhrase"))
# 변경
          .replace("// DOMAIN_AUDIO_FACTORY",
                   function(audio, "Mp3LanguageFolder") + "\n" + function(audio, "Mp3MakePhrase"))
```

- [ ] **Step 5: 통과 확인**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && python3 devices/HAS1_duct/tests/run_cooldown_tests.py 2>&1 | tail -2
```

Expected:
```
PASS: 27 firmware cooldown scenarios
PASS: 10 actual audio scheduler scenarios
```

- [ ] **Step 6: 커밋**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && git add devices/HAS1_duct/mp3_durations.h devices/HAS1_duct/audio_queue.h devices/HAS1_duct/audio_queue.ino devices/HAS1_duct/tests/audio_harness.cpp devices/HAS1_duct/tests/run_cooldown_tests.py && git commit -m "feat(duct): 폴더 09/10 개방 안내 길이표와 영어 폴더 매핑 추가

HAS2-Nextion feat/audio-library-v2 e49fa96의 09/0712·0719(한국어), 10/0712·0719(영어)
길이를 WAV 프레임 기준으로 실측해 추가. 영어 폴더는 +4 규칙에 폴더 9 → 10 예외를 두고
Mp3LanguageFolder 한 곳에서 결정한다.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: 안/밖 개방 음원 분리 (스펙 B 동작)

**Files:**
- Modify: `devices/HAS1_duct/HAS1_duct.h` (프로토타입)
- Modify: `devices/HAS1_duct/HAS1_duct_function.ino` (`OpenMp3`, `DuctOpen`, `MmmmOpen`)
- Modify: `devices/HAS1_duct/tests/run_cooldown_tests.py` (케이스 추가)
- Test: `devices/HAS1_duct/tests/host_harness.cpp` (`open_audio_paths`)

**Interfaces:**
- Consumes: `Mp3PlayLargeFolder(uint8_t, uint16_t)`, `Mp3LanguageFolder`(Task 2, 간접)
- Produces: `void OpenMp3(bool inside)` — `inside`면 09/0719, 아니면 09/0712를 큐에 넣는다.

- [ ] **Step 1: 실패하는 펌웨어 테스트 추가**

`devices/HAS1_duct/tests/host_harness.cpp`의 `} else if (test == "blockade_button_preserves_close") {` 블록 끝(`"original close still prepares frozen normal cooldown");` 다음 줄) 뒤, `} else return 2;` 앞에 삽입:

```cpp
    } else if (test == "open_audio_paths") {
        // 펌웨어 하네스 main은 mp3_available을 켜지 않는다. Mp3PlayLargeFolder가 이 플래그로 조기 반환하므로 켜 준다.
        mp3_available = true;
        openNormal();
        check(audioEvents == std::vector<String>{"play:9:712"}, "outside tag plays the outside opening line");
        advance(4000); finished();
        audioEvents.clear(); pressSwitch();
        check(relay == HIGH && audioEvents == std::vector<String>{"play:9:719"}, "inside switch plays the inside opening line");
        advance(4000); finished();
        audioEvents.clear(); DuctOpen();
        check(relay == HIGH && audioEvents == std::vector<String>{"play:9:712"}, "server manage open plays the outside opening line");
        advance(4000); finished();
        audioEvents.clear(); MmmmOpen();
        check(relay == HIGH && audioEvents == std::vector<String>{"play:9:712"}, "admin card plays the outside opening line");
        advance(4000); check(relay == LOW && !mmmm_open, "admin opening closes");
```

`devices/HAS1_duct/tests/run_cooldown_tests.py`의 `cases` 목록에서 `"blockade_remaining_audio", "blockade_reentry_audio", "blockade_button_preserves_close"]` 를 다음으로 바꾼다:

```python
         "blockade_remaining_audio", "blockade_reentry_audio", "blockade_button_preserves_close",
         "open_audio_paths"]
```

- [ ] **Step 2: 실패 확인**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && python3 devices/HAS1_duct/tests/run_cooldown_tests.py 2>&1 | grep -E 'FAIL|PASS:' | head -3
```

Expected: `FAIL at 0 ms: outside tag plays the outside opening line`

- [ ] **Step 3: `OpenMp3` 추가 후 두 개방 경로에서 사용**

`devices/HAS1_duct/HAS1_duct.h`의 `void DuctClose();` 아래에 추가:

```cpp
void OpenMp3(bool inside);
```

`devices/HAS1_duct/HAS1_duct_function.ino`의 `DuctOpen` 정의 바로 위에 추가:

```cpp
/**
 * @brief 개방 안내. 밖에서 열면(태그·서버 관리 개방·MMMM) "당겨주십시오"(09/0712),
 *        안에서 스위치로 열면 "밀어주십시오" 계열(09/0719). 영어는 Mp3MakePhrase가 폴더 10으로 바꾼다.
 */
void OpenMp3(bool inside)
{
    Mp3PlayLargeFolder(9, inside ? 719 : 712);
}
```

`DuctOpen` 안의 호출을 바꾼다:

```cpp
// 기존
        Mp3PlayLargeFolder(1, 2);
        switch_available = false;
// 변경
        OpenMp3(switch_push);
        switch_available = false;
```

`MmmmOpen` 안의 호출을 바꾼다:

```cpp
// 기존
    duct_available   = false;
    Mp3PlayLargeFolder(1, 2);
// 변경
    duct_available   = false;
    OpenMp3(false);
```

확인: `grep -n 'Mp3PlayLargeFolder(1, 2)' devices/HAS1_duct/*.ino` 결과가 `sensor.ino`의 `Mp3Check()` 한 곳만 남아야 한다(죽은 코드, 그대로 둔다).

- [ ] **Step 4: 통과 확인**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && python3 devices/HAS1_duct/tests/run_cooldown_tests.py 2>&1 | tail -2
```

Expected:
```
PASS: 28 firmware cooldown scenarios
PASS: 10 actual audio scheduler scenarios
```

- [ ] **Step 5: 커밋**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && git add devices/HAS1_duct/HAS1_duct.h devices/HAS1_duct/HAS1_duct_function.ino devices/HAS1_duct/tests/host_harness.cpp devices/HAS1_duct/tests/run_cooldown_tests.py && git commit -m "feat(duct): 안에서 스위치로 열면 별도 개방 안내(09/0719) 재생

지금은 어느 경로로 열어도 \"덕트 오픈. 손잡이를 강하게 당겨주십시오\"가 나와
안에서 열 때 문구가 맞지 않았다. 내부 스위치는 09/0719, 그 외(태그·서버 관리
개방·MMMM)는 09/0712로 통일한다. SD카드에 폴더 09/10이 있어야 한다.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: 서버 `activate`로 쿨타임 즉시 해제 (스펙 C)

**Files:**
- Modify: `devices/HAS1_duct/timer.ino` (`CooltimeFinish` 추출)
- Modify: `devices/HAS1_duct/HAS1_duct_function.ino` (`ServerActivate`)
- Modify: `devices/HAS1_duct/game_state.ino` (`DataChange`의 `activate` 분기)
- Modify: `devices/HAS1_duct/HAS1_duct.h` (프로토타입)
- Modify: `devices/HAS1_duct/tests/run_cooldown_tests.py` (`CooltimeFinish` 포함, 케이스 추가)
- Test: `devices/HAS1_duct/tests/host_harness.cpp` (`server_activate`, `server_activate_door_open`, `server_activate_blockade`)

**Interfaces:**
- Consumes: `ExitTaggerMode()`, `duct_close_timer`/`duct_close_timer_id`, `cooltime_timer`
- Produces: `void CooltimeFinish()` — 쿨타임 완료 처리(노란색, `device_state=activate` 전송, `current_time=0`, `duct_available=true`, `cool_time_neo_bool=false`, 타이머 삭제). `void ServerActivate()` — 서버 `activate` 수신 처리.

- [ ] **Step 1: 실패하는 펌웨어 테스트 추가**

`devices/HAS1_duct/tests/host_harness.cpp`의 `open_audio_paths` 블록 끝(`check(relay == LOW && !mmmm_open, "admin opening closes");` 다음) 뒤, `} else return 2;` 앞에 삽입:

```cpp
    } else if (test == "server_activate") {
        openNormal(); advance(6000); check(!duct_available && current_time == 2, "cooldown in progress");
        has2wifi.states.clear();
        ServerActivate();
        check(duct_available && current_time == 0 && !cooltime_timer.isEnabled(cooltime_timer_id),
              "server activate ends the cooldown immediately");
        check(use_duct_num == 1, "server activate keeps the use counter");
        check(pixels_line.color == std::array<int, 3>{255, 255, 0}, "server activate paints yellow");
        check(has2wifi.states == std::vector<String>{"activate"}, "server activate reports activate once");
        has2wifi.states.clear(); ServerActivate();
        check(duct_available && has2wifi.states.empty(), "server activate while available changes nothing");
        DuctTag("G1P2"); check(relay == HIGH && use_duct_num == 2, "duct opens again after forced activation");
    } else if (test == "server_activate_door_open") {
        openNormal(); advance(1000); ServerActivate();
        check(!duct_available && relay == HIGH, "server activate is ignored while the door is open");
        advance(3000); check(relay == LOW && cooltime_timer.isEnabled(cooltime_timer_id), "door still closes into a normal cooldown");
        finished();
        openNormal(); advance(6000); MmmmOpen(); advance(500); ServerActivate();
        check(mmmm_open && !duct_available, "server activate is ignored during admin opening");
        advance(3500);
        check(!mmmm_open && !duct_available && cooltime_timer.isEnabled(cooltime_timer_id), "admin close restores the cooldown");
        finished();
    } else if (test == "server_activate_blockade") {
        openNormal(); advance(6000); EnterTaggerMode(); has2wifi.states.clear();
        ServerActivate();
        check(!tagger_mode && duct_available && current_time == 0, "server activate releases blockade and cooldown together");
        check(!has2wifi.states.empty() && has2wifi.states.back() == "activate", "server reports activate after release");
        check(pixels_line.color == std::array<int, 3>{255, 255, 0}, "released duct is yellow");
        EnterTaggerMode(); has2wifi.states.clear(); ServerActivate();
        check(!tagger_mode && duct_available && has2wifi.states == std::vector<String>{"activate"},
              "available blockade release keeps existing exit behaviour");
```

`devices/HAS1_duct/tests/run_cooldown_tests.py`:

`cases` 목록의 `"open_audio_paths"]` 를 다음으로:

```python
         "open_audio_paths", "server_activate", "server_activate_door_open", "server_activate_blockade"]
```

타이머 함수 포함 줄을 다음으로:

```python
# 기존
body += "\n" + function(timer, "CooltimeTimerFunc")
# 변경
body += "\n" + function(timer, "CooltimeTimerFunc") + "\n" + function(timer, "CooltimeFinish")
```

- [ ] **Step 2: 실패 확인**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && python3 devices/HAS1_duct/tests/run_cooldown_tests.py 2>&1 | grep -E 'Missing firmware function|error:|FAIL' | head -3
```

Expected: `RuntimeError: Missing firmware function: CooltimeFinish` (러너가 함수를 못 찾음).

- [ ] **Step 3: `CooltimeFinish` 추출**

`devices/HAS1_duct/timer.ino`의 `CooltimeTimerFunc`를 아래로 교체:

```cpp
/**
 * @brief 쿨타임 완료 처리. 타이머 만료와 서버 activate(ServerActivate) 양쪽에서 공유한다.
 */
void CooltimeFinish()
{
    pixels_line.lightColor(line_yellow);
    pixels_round.lightColor(yellow);
    pixels_switch.lightColor(yellow);

    has2wifi.Send((String)(const char*)my["device_name"], "device_state", "activate");
    current_time = 0;
    duct_available = true;
    cool_time_neo_bool = false;
    cooltime_timer.deleteTimer(cooltime_timer_id);
}

void CooltimeTimerFunc()
{
    if(tagger_mode) return;   // "이로운 효과"(덕트킬 포함) 동결 중 쿨타임 일시정지 (current_time/cooltime 보존)
    if(current_time >= cooltime){
        CooltimeFinish();
    }
    else{
        current_time++;
        if(cool_time_neo_bool){
            pixels_line.clear();
            pixels_line.lightColor(line_red, NUMPIXELS_LINE * (cooltime - current_time) / cooltime);
        }
    }
}
```

`devices/HAS1_duct/HAS1_duct.h`의 `void CooltimeTimerFunc();` 위에 추가:

```cpp
void CooltimeFinish();
```

- [ ] **Step 4: `ServerActivate` 추가 후 `DataChange`에서 호출**

`devices/HAS1_duct/HAS1_duct.h`의 `void ExitTaggerMode();` 아래에 추가:

```cpp
void ServerActivate();
```

`devices/HAS1_duct/HAS1_duct_function.ino`의 `TagPlayerSend()` 정의 바로 위에 추가:

```cpp
/**
 * @brief 서버 device_state=activate 수신(운영 OS "활성화" 버튼).
 *        쿨타임 중이면 즉시 끝내고, 봉쇄 중이면 기존처럼 봉쇄를 푼다. 사용횟수 사다리는 유지.
 *        문이 열려 있는 4초(관리자 개방 포함) 동안은 건너뛴다. 이때 상태를 바꾸면
 *        닫힘 콜백이 쿨타임을 다시 시작해 표시와 실제가 어긋난다. 개방 여부는
 *        duct_close_timer 로 판단한다 - RELAY_PIN 은 OUTPUT 이라 ESP32에서 digitalRead 가
 *        항상 0을 돌려줄 수 있어 게이트로 쓸 수 없다.
 *        쿨타임이 자연 종료되어 디바이스가 보낸 activate가 되돌아오는 경우는 duct_available로 걸러진다.
 */
void ServerActivate()
{
    if (game_state == activate && !duct_available && !duct_close_timer.isEnabled(duct_close_timer_id))
        CooltimeFinish();
    ExitTaggerMode();
}
```

`devices/HAS1_duct/game_state.ino`의 `DataChange` 안 `activate` 분기:

```cpp
// 기존
        else if((String)(const char *)my["device_state"] == "activate"){
            ExitTaggerMode();
        }
// 변경
        else if((String)(const char *)my["device_state"] == "activate"){
            ServerActivate();
        }
```

`"back"` 분기의 `ExitTaggerMode();`는 그대로 둔다.

- [ ] **Step 5: 통과 확인**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && python3 devices/HAS1_duct/tests/run_cooldown_tests.py 2>&1 | tail -2
```

Expected:
```
PASS: 31 firmware cooldown scenarios
PASS: 10 actual audio scheduler scenarios
```

- [ ] **Step 6: 커밋**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && git add devices/HAS1_duct/timer.ino devices/HAS1_duct/HAS1_duct_function.ino devices/HAS1_duct/game_state.ino devices/HAS1_duct/HAS1_duct.h devices/HAS1_duct/tests/host_harness.cpp devices/HAS1_duct/tests/run_cooldown_tests.py && git commit -m "fix(duct): 서버 activate 수신 시 쿨타임 즉시 해제

운영 OS의 활성화 버튼(device_state=activate)이 봉쇄 해제만 하고 일반 쿨타임은
그대로 두던 문제. 쿨타임 완료 처리를 CooltimeFinish로 추출해 타이머 만료와
ServerActivate가 공유한다. 사용횟수는 유지하고, 문이 열려 있는 동안은 무시한다.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 5: 봉쇄 남은 시간을 서버 `left_time`으로 (스펙 D)

**Files:**
- Modify: `devices/HAS1_duct/HAS1_duct.h` (전역 3개, 프로토타입)
- Modify: `devices/HAS1_duct/HAS1_duct_function.ino` (`TaggerLeftTimeUpdate`, `TaggerRemainingSeconds`)
- Modify: `devices/HAS1_duct/game_state.ino` (`EnterTaggerMode`, `DataChange`)
- Modify: `devices/HAS1_duct/tests/run_cooldown_tests.py` (케이스 추가)
- Modify: `devices/HAS1_duct/tests/README.md` (시나리오 수·설명)
- Test: `devices/HAS1_duct/tests/host_harness.cpp` (`blockade_left_time`)

**Interfaces:**
- Consumes: `tagger_mode`, `tagger_started_ms`, `tagger_duration_ms`, `my["left_time"]`
- Produces: 전역 `int tagger_left_time_s`, `unsigned long tagger_left_time_ms`, `bool tagger_left_time_valid`; `void TaggerLeftTimeUpdate()`; 갱신된 `int TaggerRemainingSeconds()`.

- [ ] **Step 1: 실패하는 펌웨어 테스트 추가**

`devices/HAS1_duct/tests/host_harness.cpp`의 `server_activate_blockade` 블록 끝(`"available blockade release keeps existing exit behaviour");` 다음) 뒤, `} else return 2;` 앞에 삽입:

```cpp
    } else if (test == "blockade_left_time") {
        EnterTaggerMode(); TaggerLeftTimeUpdate();
        check(TaggerRemainingSeconds() == 30, "missing left_time keeps the 30 second default");
        my["left_time"] = "25"; TaggerLeftTimeUpdate();
        check(TaggerRemainingSeconds() == 25, "server left_time replaces the default");
        advance(3000); check(TaggerRemainingSeconds() == 22, "remaining time counts down from the last received value");
        TaggerLeftTimeUpdate();   // 같은 값(25) 재수신 - 수신 시각을 다시 찍으면 안 된다
        check(TaggerRemainingSeconds() == 22, "repeated identical left_time does not rewind the countdown");
        expectBlockadeAudio(22);
        my["left_time"] = "10"; TaggerLeftTimeUpdate(); advance(500);
        check(TaggerRemainingSeconds() == 10, "newer left_time wins and partial seconds round up");
        my["left_time"] = "0"; TaggerLeftTimeUpdate();
        check(TaggerRemainingSeconds() == 10, "zero left_time does not overwrite the last value");
        advance(12000);
        check(TaggerRemainingSeconds() == 0 && tagger_mode, "expired server value announces zero but never self-releases");
        ExitTaggerMode(); my["left_time"] = ""; EnterTaggerMode(); TaggerLeftTimeUpdate();
        check(TaggerRemainingSeconds() == 30, "re-entering the blockade forgets the previous server value");
        ExitTaggerMode(); my["left_time"] = "7"; TaggerLeftTimeUpdate();
        check(!tagger_left_time_valid, "left_time is ignored outside the blockade");
```

`devices/HAS1_duct/tests/run_cooldown_tests.py`의 `cases` 목록에서 `"server_activate_blockade"]` 를 다음으로:

```python
         "open_audio_paths", "server_activate", "server_activate_door_open", "server_activate_blockade",
         "blockade_left_time"]
```

- [ ] **Step 2: 실패 확인**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && python3 devices/HAS1_duct/tests/run_cooldown_tests.py 2>&1 | grep -E 'error:|FAIL' | head -3
```

Expected: clang++ 컴파일 에러 `use of undeclared identifier 'TaggerLeftTimeUpdate'`.

- [ ] **Step 3: 전역·프로토타입 추가**

`devices/HAS1_duct/HAS1_duct.h`:

```cpp
// 기존
// TODO: 서버의 봉쇄 시간 값을 받으면 이 기본값을 대체한다. 해제는 서버 명령으로만 처리한다.
unsigned long tagger_duration_ms = 30000UL;
unsigned long tagger_started_ms = 0;
// 변경
// 봉쇄 남은 시간은 서버 left_time(초, 서버가 카운트다운)을 우선 쓰고, 아직 못 받았으면 30초 기본값.
// 해제는 서버 명령(back/activate)으로만 처리한다.
unsigned long tagger_duration_ms = 30000UL;
unsigned long tagger_started_ms = 0;
int tagger_left_time_s = 0;              // 마지막으로 받은 서버 left_time(초)
unsigned long tagger_left_time_ms = 0;   // 그 값을 받은 시각(millis)
bool tagger_left_time_valid = false;     // 이번 봉쇄에서 서버 값을 받았는지
```

`int TaggerRemainingSeconds();` 아래에 추가:

```cpp
void TaggerLeftTimeUpdate();
```

- [ ] **Step 4: 함수 구현**

`devices/HAS1_duct/HAS1_duct_function.ino`의 `TaggerRemainingSeconds`를 아래로 교체하고 그 위에 `TaggerLeftTimeUpdate`를 둔다:

```cpp
/**
 * @brief 서버 폴링마다 호출. 봉쇄 중이고 left_time(초)이 양수이며 직전 값과 다를 때만
 *        값과 수신 시각을 저장한다. 0 이하·부재는 무시한다.
 *        같은 값에 수신 시각을 다시 찍으면 폴링마다 카운트다운이 되감겨,
 *        서버가 같은 값을 반복해 보내는 동안 남은 시간이 그 값에서 멈춘다.
 */
void TaggerLeftTimeUpdate()
{
    if (!tagger_mode) return;
    int left_time = (int)my["left_time"];
    if (left_time <= 0) return;
    if (tagger_left_time_valid && left_time == tagger_left_time_s) return;
    tagger_left_time_s = left_time;
    tagger_left_time_ms = millis();
    tagger_left_time_valid = true;
}

int TaggerRemainingSeconds()
{
    unsigned long elapsed_ms;
    unsigned long total_ms;
    if (tagger_left_time_valid)
    {
        elapsed_ms = millis() - tagger_left_time_ms;
        total_ms = (unsigned long)tagger_left_time_s * 1000UL;
    }
    else
    {
        elapsed_ms = millis() - tagger_started_ms;
        total_ms = tagger_duration_ms;
    }
    if (elapsed_ms >= total_ms) return 0;
    unsigned long remaining_ms = total_ms - elapsed_ms;
    return remaining_ms / 1000UL + (remaining_ms % 1000UL != 0);
}
```

`devices/HAS1_duct/game_state.ino`의 `EnterTaggerMode`:

```cpp
// 기존
    tagger_mode = true;        // RfidLoop / CooltimeTimerFunc 자동 정지
    tagger_started_ms = millis();
// 변경
    tagger_mode = true;        // RfidLoop / CooltimeTimerFunc 자동 정지
    tagger_started_ms = millis();
    tagger_left_time_valid = false;   // 새 봉쇄: 서버 left_time을 다시 받기 전까지 30초 기본값
```

`devices/HAS1_duct/game_state.ino`의 `DataChange`에서 `if(!tagger_mode){` 블록 바로 위(game_state 전환 블록 뒤)에 추가:

```cpp
    // 봉쇄 남은 시간: 서버 left_time을 폴링마다 반영 (device_state=tagger 와 같은 응답에 와도 위에서
    // EnterTaggerMode 가 먼저 실행되므로 여기서 바로 잡힌다)
    TaggerLeftTimeUpdate();
```

- [ ] **Step 5: 통과 확인**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && python3 devices/HAS1_duct/tests/run_cooldown_tests.py 2>&1 | tail -2
```

Expected:
```
PASS: 32 firmware cooldown scenarios
PASS: 10 actual audio scheduler scenarios
```

- [ ] **Step 6: 테스트 README 갱신**

`devices/HAS1_duct/tests/README.md`:

```
기존: - **23 device scenarios:** actual firmware state transitions, button logic and phrase builders, with queue submission recorded immediately.
변경: - **32 device scenarios:** actual firmware state transitions, button logic, opening-line selection, server activate handling, server `left_time` blockade countdown and phrase builders, with queue submission recorded immediately.

기존: - **9 audio scenarios:** actual `audio_queue.ino`, phrase builders and measured `mp3_durations.h`, with only DFPlayer hardware replaced.
변경: - **10 audio scenarios:** actual `audio_queue.ino`, phrase builders and measured `mp3_durations.h`, with only DFPlayer hardware replaced.

기존: The duration table targets `HAS2-Nextion` branch `feat/audio-library-v2`, `audios/V2/duct_MP3_22k/01~08`; its source commit is recorded in the header.
변경: The duration table targets `HAS2-Nextion` branch `feat/audio-library-v2`, `audios/V2/duct_MP3_22k/01~08` plus the opening lines `09/0712`, `09/0719`, `10/0712`, `10/0719`; its source commit is recorded in the header.
```

- [ ] **Step 7: 커밋**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && git add devices/HAS1_duct/HAS1_duct.h devices/HAS1_duct/HAS1_duct_function.ino devices/HAS1_duct/game_state.ino devices/HAS1_duct/tests/host_harness.cpp devices/HAS1_duct/tests/run_cooldown_tests.py devices/HAS1_duct/tests/README.md && git commit -m "feat(duct): 봉쇄 남은 시간을 서버 left_time으로 계산

30초 하드코딩 대신 폴링마다 받은 my[\"left_time\"](초, 서버 카운트다운)에서
경과 시간을 빼 안내한다. 아직 못 받았거나 0 이하면 기존 30초 기본값. 봉쇄 해제는
여전히 서버 명령으로만 한다.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 6: 최종 검증과 배포 준비

**Files:** 변경 없음(검증·푸시만)

- [ ] **Step 1: 전체 테스트와 변경 요약**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && python3 devices/HAS1_duct/tests/run_cooldown_tests.py 2>&1 | tail -2 && git log --oneline main..HEAD && git diff --stat main..HEAD -- devices/HAS1_duct
```

Expected: `PASS: 32 …` / `PASS: 10 …`, 커밋 7개(설계 문서 1 + 계획 문서 1 + 구현 5), `devices/HAS1_duct` 변경 파일이 이 계획의 파일 구조 표에 있는 것들뿐임.

- [ ] **Step 2: `Mp3PlayLargeFolder(1, 2)` 잔존 확인**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && grep -n 'Mp3PlayLargeFolder(1, 2)' devices/HAS1_duct/*.ino
```

Expected: `sensor.ino`의 `Mp3Check()` 한 줄만.

- [ ] **Step 3: 브랜치 푸시**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && git push -u origin claude/duct-polish
```

- [ ] **Step 4: 배포 전 충돌 확인 후 사용자 확인 받고 Deploy Firmware 실행**

다른 브랜치가 최근에 `HAS1_duct`를 배포했는지 확인(같은 vN이 두 번 나오면 장치가 OTA를 건너뛴다):

```bash
gh run list --workflow deploy-firmware.yml --limit 10 --json headBranch,createdAt,displayTitle --jq '.[] | select(.displayTitle | contains("HAS1_duct")) | "\(.createdAt[:16]) \(.headBranch)"'
```

Expected: 오늘 날짜의 duct 배포는 `claude/neopixel-brightness-unify`(이미 main에 머지됨)만 있어야 한다.

배포는 현장 OTA 대상 릴리즈 자산을 바꾸는 외부 영향 작업이므로 **사용자에게 실행 여부를 확인한 뒤** 단일 명령으로 실행한다:

```bash
gh workflow run deploy-firmware.yml --ref claude/duct-polish -f device=HAS1_duct -f update_partition=false
```

CI가 `FIRMWARE_VER`를 53으로 올린 `Firmware v53` 커밋을 브랜치에 푸시하고 릴리즈 태그 `HAS1_duct`의 자산을 교체한다. 이후 `git pull`로 그 커밋을 받는다.

- [ ] **Step 5: PR 생성(사용자 확인 후)**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && gh pr create --base main --head claude/duct-polish --title "HAS1_duct: 폴리싱 4건 (안내 딜레이·안/밖 개방 음원·OS 활성화·left_time)" --body "$(cat <<'EOF'
## Summary
- 안내 트랙 사이 여유 200ms → 100ms
- 내부 스위치 개방은 09/0719, 그 외 개방은 09/0712 (영어 폴더 10). SD카드에 폴더 09/10 필요
- 서버 device_state=activate 수신 시 쿨타임 즉시 해제 (사용횟수 유지, 문 열림 중 무시)
- 봉쇄 남은 시간을 서버 left_time(초)으로 계산, 미수신 시 30초 기본값

설계: docs/superpowers/specs/2026-09-19-duct-polish-design.md

## Test plan
- [x] `python3 devices/HAS1_duct/tests/run_cooldown_tests.py` 32 + 10 시나리오 통과
- [ ] 실기: 쿨타임 중 태그 → "해제까지"와 숫자 사이 공백 감소, 끝 잘림 없음
- [ ] 실기: 내부 스위치 개방 → 0719 재생, 태그/MMMM 개방 → 0712 재생
- [ ] 실기: 쿨타임 중 OS 활성화 → 즉시 노란색·사용 가능
- [ ] 실기: 봉쇄 중 태그 → 서버 left_time 기준 남은 초 안내

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

OTA 시작(`device_state=github`)은 사용자가 게임 서버에서 직접 한다.
