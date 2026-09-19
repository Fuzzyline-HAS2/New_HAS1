# iotglove vibe 연출 명령(10~17) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 서버 `vibe` 필드의 10(음소거)·11(연속 ON)·12~14(짧은 진동 1~3회)·15~17(긴 진동 1~3회)을 iotglove 펌웨어가 상태·역할과 무관하게 해석해 모터로 출력하고, v8로 릴리즈한다.

**Architecture:** 모터 결정은 전부 `devices/iotglove/feedback.h`의 `FeedbackEngine::update()` 한 곳에서 한다. `feedback_config.h`의 `Schedule`을 "펄스 N회 주기형"으로 일반화해 기존 상태전환 패턴(Short1/Long1/Short2)과 새 명령 패턴이 같은 `motorOn()`을 쓰게 하고, 엔진은 `lastVibe_`로 값 변화(엣지)를 감지해 12~17을 1회 재생하며 10/11은 유지형 레벨로 처리한다. `wifi.cpp`는 허용 범위만 0~3 → 0~17로 넓힌다.

**Tech Stack:** C++17 (호스트 테스트: `c++ -Wall -Wextra -Werror -pedantic`), Arduino ESP32 core 3.3.11 (TTGO T1), arduino-cli, GitHub Actions `Deploy Firmware` 워크플로, `gh` CLI.

**Spec:** `docs/superpowers/specs/2026-09-19-iotglove-vibe-commands-design.md`

## Global Constraints

- 작업 브랜치: `claude/iotglove-vibe-commands` (main 기준, 스펙 커밋 `d82cfc8` 이후). 모든 커밋은 이 브랜치에.
- 서버 `vibe` 허용 범위는 **0~17 정수**. 그 밖(18 이상·음수·누락)은 기존대로 스냅샷 거부.
- 명령 진동 길이: **짧 200ms / 길 600ms / 간격 200ms**. 상태전환 진동(150/300/100ms)은 **변경 금지**.
- 10~17은 `game_state`·`device_state`·`role` 무관. 유일 예외: OTA/리셋 중 모터 OFF(기존 `iotglove.cpp:455`).
- 우선순위: 10 음소거 > 11 ON > 칩 제거/발견 이벤트 > 명령 패턴 > 상태전환 패턴 > 근접 펄스.
- `FIRMWARE_VER`은 **수동으로 올리지 않는다** — CI(`ci_deploy.py bump`)가 7 → 8로 올린다.
- iotglove 코드 주석은 **영문**(기존 컨벤션). 문서(README/SERVER_CONTRACT)는 한국어.
- 호스트 테스트는 하드웨어 없이 `python3 devices/iotglove/tools/run_tests.py`로 전부 통과해야 한다.
- 커밋 메시지 끝: `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`

## 공통 명령 (각 태스크의 "실행" 단계에서 반복 사용)

`feedback_state_test.cpp` 하나만 빠르게 빌드·실행:

```bash
cd /Users/byeongjun/workspace/New_HAS1 && OUT=$(mktemp -d) && c++ -std=c++17 -Wall -Wextra -Werror -pedantic -I libraries/IoTGloveProtocol/src -I devices/iotglove devices/iotglove/tests/feedback_state_test.cpp devices/iotglove/game_state.cpp -o "$OUT/t" && "$OUT/t"
```

전체 호스트 테스트(15개 실행파일):

```bash
cd /Users/byeongjun/workspace/New_HAS1 && python3 devices/iotglove/tools/run_tests.py 2>&1 | tail -4
```

## File Structure

| 파일 | 책임 | 변경 |
|---|---|---|
| `devices/iotglove/feedback_config.h` | 진동 패턴의 **모양**(길이·간격·횟수)과 설정값. 순수 함수만 | `Schedule` 주기형 일반화, `pulses()`, `commandSchedule()`, `isCommand()`, 상수, `Settings` 3필드 |
| `devices/iotglove/feedback.h` | **언제 어떤 패턴을 시작·취소하는지**(우선순위·엣지·레벨). 상태 보유 | `Source::Command`, `lastVibe_`, `haveVibe_`, `start(Schedule)`, `update()` 규칙 |
| `devices/iotglove/wifi.cpp` | 서버 JSON → `ServerSnapshot` 파싱 (Arduino 전용) | `vibe` 범위 0~17, `feedback_config.h` include |
| `devices/iotglove/tests/feedback_state_test.cpp` | 엔진·패턴 호스트 테스트 | 함수 3개 추가, `main()` 등록 |
| `devices/iotglove/docs/SERVER_CONTRACT.md` | 서버 계약 | `vibe` 행 갱신 + 연출 명령 절 추가 |
| `devices/iotglove/README.md` | 운영 설명 | 근접 진동 문단에 명령 진동 문장 추가 |

`game_state.h`(`ServerSnapshot.vibe`는 `uint8_t`)·`iotglove.cpp`(`render()`)는 변경 없음.

---

### Task 1: `Schedule` 주기형 일반화 + 명령 패턴 빌더 (`feedback_config.h`)

**Files:**
- Modify: `devices/iotglove/feedback_config.h:12-59`
- Test: `devices/iotglove/tests/feedback_state_test.cpp`

**Interfaces:**
- Consumes: 없음 (기존 `Pattern`, `Settings.shortMs/longMs/doubleGapMs`)
- Produces (Task 2·3·4가 사용):
  - `struct Schedule { uint32_t onMs, gapMs, count, total; }` — `firstOn/gap/secondOn` 필드는 **삭제**
  - `Schedule pulses(uint32_t onMs, uint32_t gapMs, uint32_t count)`
  - `Schedule commandSchedule(uint8_t vibe, const Settings&)` — 12~17만 비어 있지 않은 스케줄
  - `bool isCommand(uint8_t vibe)` — 12 ≤ vibe ≤ 17
  - `constexpr uint8_t kVibeMute = 10, kVibeOn = 11, kVibeCommandFirst = 12, kVibeCommandLast = 17;`
  - `Settings.commandShortMs = 200, commandLongMs = 600, commandGapMs = 200`
  - `bool motorOn(const Schedule&, uint32_t elapsed)` — 시그니처 동일, 주기형으로 재구현

- [ ] **Step 1: 실패하는 테스트 작성**

`devices/iotglove/tests/feedback_state_test.cpp`의 `wrapAndTaggerLeds()` 함수 **뒤**, `int main()` **앞**에 추가:

```cpp
static void commandSchedules() {
  Settings config;
  auto s = feedback_config::pulses(200, 200, 3);
  assert(s.total == 1000 && s.count == 3 && s.onMs == 200 && s.gapMs == 200);
  assert(feedback_config::motorOn(s, 0) && feedback_config::motorOn(s, 199));
  assert(!feedback_config::motorOn(s, 200) && !feedback_config::motorOn(s, 399));
  assert(feedback_config::motorOn(s, 400) && feedback_config::motorOn(s, 599));
  assert(!feedback_config::motorOn(s, 600) && !feedback_config::motorOn(s, 799));
  assert(feedback_config::motorOn(s, 800) && feedback_config::motorOn(s, 999));
  assert(!feedback_config::motorOn(s, 1000));
  assert(feedback_config::pulses(0, 200, 3).total == 0);       // Zero ON disables.
  assert(feedback_config::pulses(200, 200, 0).total == 0);     // Zero count disables.
  assert(feedback_config::pulses(UINT32_MAX, 0, 2).total == 0);  // Overflow rejected.
  assert(feedback_config::pulses(300, 0, 1).total == 300);

  assert(feedback_config::commandSchedule(12, config).total == 200);
  assert(feedback_config::commandSchedule(13, config).total == 600);
  assert(feedback_config::commandSchedule(14, config).total == 1000);
  assert(feedback_config::commandSchedule(15, config).total == 600);
  assert(feedback_config::commandSchedule(16, config).total == 1400);
  assert(feedback_config::commandSchedule(17, config).total == 2200);
  assert(feedback_config::commandSchedule(0, config).total == 0);
  assert(feedback_config::commandSchedule(11, config).total == 0);
  assert(feedback_config::commandSchedule(18, config).total == 0);
  assert(feedback_config::isCommand(12) && feedback_config::isCommand(17));
  assert(!feedback_config::isCommand(11) && !feedback_config::isCommand(18));
  assert(feedback_config::kVibeMute == 10 && feedback_config::kVibeOn == 11);

  // Existing state patterns keep their exact shape on the generalized schedule.
  s = feedback_config::schedule(Pattern::Short2, config);
  assert(s.onMs == 150 && s.gapMs == 100 && s.count == 2 && s.total == 400);
  s = feedback_config::schedule(Pattern::Long1, config);
  assert(s.onMs == 300 && s.gapMs == 0 && s.count == 1 && s.total == 300);
}
```

`int main()`을 아래로 교체(새 함수 등록):

```cpp
int main() {
  patternsAndConfiguration();
  semanticTransitionsAndNoReplay();
  prioritiesAndCancellation();
  onlyServerRoleTransitionsSignal();
  wrapAndTaggerLeds();
  commandSchedules();
  puts("PASS: configured state haptics, priorities, reconnect suppression, authoritative roles, tagger LEDs and operator vibe commands");
}
```

- [ ] **Step 2: 실패 확인**

Run: 공통 명령(단일 테스트)
Expected: **컴파일 실패** — `no member named 'pulses' in namespace 'iotglove::feedback_config'` (또는 `commandSchedule`/`isCommand`/`kVibeMute` 미정의)

- [ ] **Step 3: 구현**

`devices/iotglove/feedback_config.h`에서 `struct Settings { ... };`부터 `inline bool motorOn(...) { ... }`까지(현재 12~59행)를 아래로 교체. `forState()`(61행 이후)는 그대로 둔다.

```cpp
struct Settings {
  uint32_t shortMs = 150;
  uint32_t longMs = 300;
  uint32_t doubleGapMs = 100;

  Pattern onSetting = Pattern::Short1;
  Pattern onReady = Pattern::Short1;
  Pattern onExploration = Pattern::Short1;
  Pattern onPlayer = Pattern::Short1;
  Pattern onGhost = Pattern::Long1;
  Pattern onTaggerBlink = Pattern::Short2;
  Pattern onTaggerActive = Pattern::Long1;
  Pattern onEnded = Pattern::Long1;

  // Preserve the existing explicit-event timings: 300 and 150/100/150 ms.
  Pattern onRemoved = Pattern::Long1;
  Pattern onFound = Pattern::Short2;

  // Operator vibe commands (server vibe 12..17) are tuned apart from state patterns.
  uint32_t commandShortMs = 200;
  uint32_t commandLongMs = 600;
  uint32_t commandGapMs = 200;
};

// Server vibe values that are operator commands rather than proximity levels (0/1/3).
constexpr uint8_t kVibeMute = 10;          // Hold: motor never runs.
constexpr uint8_t kVibeOn = 11;            // Hold: motor runs continuously.
constexpr uint8_t kVibeCommandFirst = 12;  // 12..14 short x1..3, 15..17 long x1..3, once per edge.
constexpr uint8_t kVibeCommandLast = 17;

inline bool isCommand(uint8_t vibe) { return vibe >= kVibeCommandFirst && vibe <= kVibeCommandLast; }

// A pulse train: `count` pulses of `onMs`, separated by `gapMs`. `total` ends with the last ON.
struct Schedule {
  uint32_t onMs = 0;
  uint32_t gapMs = 0;
  uint32_t count = 0;
  uint32_t total = 0;
};

inline Schedule pulses(uint32_t onMs, uint32_t gapMs, uint32_t count) {
  Schedule out;
  // Zero disables the pulse; reject overflowing/ambiguous millis durations.
  if (!onMs || !count) return out;
  const uint64_t total = uint64_t(onMs) * count + uint64_t(gapMs) * (count - 1);
  if (total > 0x7fffffffULL) return out;
  out.onMs = onMs;
  out.gapMs = gapMs;
  out.count = count;
  out.total = uint32_t(total);
  return out;
}

inline Schedule schedule(Pattern pattern, const Settings& settings) {
  switch (pattern) {
    case Pattern::Short1: return pulses(settings.shortMs, 0, 1);
    case Pattern::Long1: return pulses(settings.longMs, 0, 1);
    case Pattern::Short2: return pulses(settings.shortMs, settings.doubleGapMs, 2);
    case Pattern::Off: break;
  }
  return Schedule{};
}

inline Schedule commandSchedule(uint8_t vibe, const Settings& settings) {
  if (vibe >= 12 && vibe <= 14) return pulses(settings.commandShortMs, settings.commandGapMs, uint32_t(vibe - 11));
  if (vibe >= 15 && vibe <= 17) return pulses(settings.commandLongMs, settings.commandGapMs, uint32_t(vibe - 14));
  return Schedule{};
}

inline bool motorOn(const Schedule& pattern, uint32_t elapsed) {
  // An empty schedule has total 0, so the division below never sees a zero period.
  return elapsed < pattern.total && elapsed % (pattern.onMs + pattern.gapMs) < pattern.onMs;
}
```

- [ ] **Step 4: 통과 확인**

Run: 공통 명령(단일 테스트)
Expected: `PASS: configured state haptics, priorities, reconnect suppression, authoritative roles, tagger LEDs and operator vibe commands` — 기존 `patternsAndConfiguration()`의 Short1/Long1/Short2 경계 assert가 수정 없이 통과해야 한다.

Run: 공통 명령(전체)
Expected: 마지막 줄 `Passed 15 firmware host test executables`

- [ ] **Step 5: 커밋**

```bash
cd /Users/byeongjun/workspace/New_HAS1
git add devices/iotglove/feedback_config.h devices/iotglove/tests/feedback_state_test.cpp
git commit -m "feat(iotglove): generalize haptic schedule to pulse trains and add vibe command patterns" -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: 엔진 — 명령 엣지(12~17)와 레벨(10/11) (`feedback.h`)

**Files:**
- Modify: `devices/iotglove/feedback.h:18-83` (`update()` 전체), `:85-111` (private 멤버·헬퍼)
- Test: `devices/iotglove/tests/feedback_state_test.cpp`

**Interfaces:**
- Consumes (Task 1): `feedback_config::isCommand`, `commandSchedule`, `kVibeMute`, `kVibeOn`, `Schedule`
- Produces (Task 3가 확장): `enum class Source { None, State, Event, Command }`, `uint8_t lastVibe_`, `bool haveVibe_`, `void start(const feedback_config::Schedule&, Source, uint32_t)`. `update()` 시그니처는 변경 없음.

- [ ] **Step 1: 실패하는 테스트 작성**

`commandSchedules()` 뒤에 추가:

```cpp
static void commandEdgesAndLevels() {
  FeedbackEngine engine;
  auto f = state(Role::Player, DeviceState::Activate, Phase::Active, Display::Player);
  assert(!engine.update(f, 0, false, 0).motor);       // Baseline.
  assert(engine.update(f, 12, false, 1000).motor);    // 0 -> 12 edge: short x1 (200 ms).
  assert(engine.update(f, 12, false, 1199).motor);
  assert(!engine.update(f, 12, false, 1200).motor);
  assert(!engine.update(f, 12, false, 2000).motor);   // A held value never restarts.
  assert(engine.update(f, 13, false, 3000).motor);    // 12 -> 13 edge: short x2.
  assert(!engine.update(f, 13, false, 3200).motor);
  assert(engine.update(f, 13, false, 3400).motor);
  assert(!engine.update(f, 13, false, 3600).motor);
  assert(!engine.update(f, 0, false, 4000).motor);    // Back to 0: silence.
  assert(engine.update(f, 13, false, 5000).motor);    // Same command replays after a 0 gap.
  assert(!engine.update(f, 13, false, 5600).motor);
  assert(engine.update(f, 15, false, 6000).motor);    // Long x1: 600 ms.
  assert(engine.update(f, 15, false, 6599).motor);
  assert(!engine.update(f, 15, false, 6600).motor);
  assert(!engine.update(f, 7, false, 7000).motor);    // 4..9 behave like 0.
  assert(!engine.update(f, 2, false, 7100).motor);

  assert(engine.update(f, 11, false, 8000).motor);    // 11 holds the motor on...
  assert(engine.update(f, 11, false, 9000).motor);
  assert(!engine.update(f, 0, false, 10000).motor);   // ...until released.
  assert(engine.update(f, 17, false, 11000).motor);   // Long x3 running (ON 11000-11599, 11800-12399)...
  assert(!engine.update(f, 10, false, 11100).motor);  // ...mute cancels it.
  assert(!engine.update(f, 10, false, 11500).motor);
  assert(!engine.update(f, 0, false, 11900).motor);   // Released mute: the cancelled train does not resume.
  assert(engine.update(f, 14, false, 12000).motor);   // Short x3 (ON 12000-12199, 12400-12599, 12800-12999)...
  assert(engine.update(f, 11, false, 12100).motor);   // ...replaced by continuous ON...
  assert(!engine.update(f, 0, false, 12450).motor);   // ...and it does not resume when ON is released.
}
```

`int main()`에 `commandSchedules();` 다음 줄로 `commandEdgesAndLevels();` 추가.

- [ ] **Step 2: 실패 확인**

Run: 공통 명령(단일 테스트)
Expected: 컴파일은 성공하나 `Assertion failed: (engine.update(f, 12, false, 1000).motor)` 로 종료(엔진이 아직 12를 무시함)

- [ ] **Step 3: 구현**

`devices/iotglove/feedback.h`의 `update()` 함수 전체(18~83행)를 아래로 교체:

```cpp
  Outputs update(const Feedback& state, uint8_t vibe, bool locationFresh, uint32_t now,
                 bool suppressed = false) {
    Outputs out;
    out.lit = state.lit;
    switch (state.display) {
      case Display::Setting: out.red = out.green = out.blue = 32; break;
      case Display::Ready:
      case Display::Ended: out.red = 64; break;
      case Display::Player: out.green = 64; break;
      case Display::Ghost: out.blue = 64; break;
      case Display::Tagger:
      case Display::TaggerActive: out.red = 48; out.blue = 64; break;
      case Display::TaggerBlink:
        if (display_ != Display::TaggerBlink) blinkStart_ = now;
        if ((uint32_t(now - blinkStart_) / 500U) % 2U == 0) { out.red = 48; out.blue = 64; }
        break;
    }

    if (!state.stateValid || suppressed) {
      cancel();
      pendingGhostAck_ = false;
      known_ = state.stateValid;
      remember(state);
      return out;
    }
    const bool baseline = !known_ || state.stateEpoch != epoch_;
    const bool changed = !baseline && (state.role != role_ ||
        state.deviceState != deviceState_ || state.phase != phase_);
    const bool removedAcknowledged = changed && pendingGhostAck_ &&
        role_ == Role::Player && state.role == Role::Ghost &&
        state.deviceState == deviceState_ && state.phase == phase_;
    if (baseline) {
      cancel();
      pendingGhostAck_ = false;
    }
    if (pattern_.total && uint32_t(now - patternStart_) >= pattern_.total) cancel();
    // Stop any preceding activity before a new setting/ready/end notification.
    // This is an edge, not a per-poll cancellation of the new notification.
    if ((changed && quiescent(state)) ||
        (state.display == Display::Ended && display_ != Display::Ended)) cancel();
    if (changed) pendingGhostAck_ = false;

    // Operator vibe commands. 10/11 are levels that hold while the server keeps the value;
    // 12..17 play once per change of value (the server must pass through another value to repeat).
    const bool vibeEdge = vibe != lastVibe_;
    lastVibe_ = vibe;
    if (vibe == feedback_config::kVibeMute || vibe == feedback_config::kVibeOn) {
      if (vibeEdge) cancel();  // Nothing queued resumes once the level is released.
      known_ = true;
      remember(state);
      out.motor = vibe == feedback_config::kVibeOn;
      return out;
    }
    if (vibeEdge && feedback_config::isCommand(vibe)) {
      start(feedback_config::commandSchedule(vibe, settings_), Source::Command, now);
    }

    if (state.haptic != Haptic::None) {
      const auto choice = state.haptic == Haptic::Removed ? settings_.onRemoved : settings_.onFound;
      start(choice, Source::Event, now);
      // A capture's later server acknowledgement must not repeat its vibration.
      if (state.haptic == Haptic::Removed && pattern_.total && state.role == Role::Player &&
          state.phase == Phase::Active) pendingGhostAck_ = true;
    } else if (changed && !removedAcknowledged && source_ != Source::Event) {
      // Consume transitions during explicit events; never queue a stale replay.
      start(feedback_config::forState(state, settings_), Source::State, now);
    }
    known_ = true;
    remember(state);

    if (pattern_.total) {
      // The OFF gap is part of the pattern and also overrides proximity pulses.
      out.motor = feedback_config::motorOn(pattern_, uint32_t(now - patternStart_));
    } else if (locationFresh && state.display == Display::Player) {
      if (vibe == 3) {
        const uint32_t t = now % 1000U;
        out.motor = t < 100U || (t >= 200U && t < 300U);
      } else if (vibe == 1) out.motor = now % 2000U < 100U;
    }
    return out;
  }
```

private 영역(85~111행)의 `enum class Source ...`부터 `void start(...) { ... }`까지를 아래로 교체(`remember()`는 그대로):

```cpp
 private:
  enum class Source : uint8_t { None, State, Event, Command };
  feedback_config::Settings settings_;
  feedback_config::Schedule pattern_;
  uint32_t patternStart_ = 0;
  uint32_t blinkStart_ = 0;
  Source source_ = Source::None;
  bool known_ = false;
  bool pendingGhostAck_ = false;
  Role role_ = Role::Neutral;
  DeviceState deviceState_ = DeviceState::Other;
  Phase phase_ = Phase::Unknown;
  Display display_ = Display::Setting;
  uint32_t epoch_ = 0;
  uint8_t lastVibe_ = 0;   // Last server vibe seen on a valid poll; commands play on change only.
  bool haveVibe_ = false;  // False until the first valid poll (Task 3 uses this for baseline consumption).

  static bool quiescent(const Feedback& state) {
    return state.phase == Phase::Setting || state.phase == Phase::Ready ||
        state.phase == Phase::Exploration || state.phase == Phase::Ended ||
        state.deviceState == DeviceState::Setting || state.deviceState == DeviceState::Ready ||
        state.deviceState == DeviceState::Exploration || state.deviceState == DeviceState::Ended;
  }
  void cancel() { pattern_ = feedback_config::Schedule{}; source_ = Source::None; }
  void start(const feedback_config::Schedule& schedule, Source source, uint32_t now) {
    pattern_ = schedule;
    source_ = pattern_.total ? source : Source::None;
    patternStart_ = now;
  }
  void start(feedback_config::Pattern pattern, Source source, uint32_t now) {
    start(feedback_config::schedule(pattern, settings_), source, now);
  }
```

- [ ] **Step 4: 통과 확인**

Run: 공통 명령(단일 테스트)
Expected: `PASS: ... operator vibe commands`

Run: 공통 명령(전체)
Expected: `Passed 15 firmware host test executables`. `-Wextra -Werror`에서 `haveVibe_` 미사용 경고는 나지 않는다(private 데이터 멤버는 -Wunused 대상이 아님). 경고가 나면 Task 3에서 사용되므로 임시로 `(void)haveVibe_;`를 넣지 말고 Task 3를 이어서 진행한다.

- [ ] **Step 5: 커밋**

```bash
cd /Users/byeongjun/workspace/New_HAS1
git add devices/iotglove/feedback.h devices/iotglove/tests/feedback_state_test.cpp
git commit -m "feat(iotglove): play server vibe commands 12-17 once per edge and hold 10/11 as levels" -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: 엔진 — 우선순위와 예외 규칙 (`feedback.h`)

**Files:**
- Modify: `devices/iotglove/feedback.h` (`update()` 4곳)
- Test: `devices/iotglove/tests/feedback_state_test.cpp`

**Interfaces:**
- Consumes (Task 2): `Source::Command`, `lastVibe_`, `haveVibe_`
- Produces: 없음 (동작 규칙만 완성)

규칙(스펙 §3): ① 칩 이벤트는 명령을 끊는다(기존 무조건 `start(Event)`로 이미 성립) ② 상태전환은 명령을 끊지 못한다 — 상태 시작 검사와 정지상태 `cancel()` 둘 다에서 `Command` 제외 ③ 무효 폴은 `lastVibe_`를 건드리지 않는다(Task 2 구조로 이미 성립) ④ 억제(OTA/리셋) 중 유효 스냅샷의 vibe는 소비 ⑤ 첫 동기화·epoch 변경(baseline)의 vibe는 소비, 단 일시 무효 뒤 같은 epoch의 baseline은 `lastVibe_` 유지.

- [ ] **Step 1: 실패하는 테스트 작성**

`commandEdgesAndLevels()` 뒤에 추가:

```cpp
static void commandPrioritiesAndExceptions() {
  FeedbackEngine engine;
  auto f = state(Role::Player, DeviceState::Activate, Phase::Active, Display::Player);
  engine.update(f, 0, false, 0);  // Baseline.

  // Chip events interrupt a running command: a game signal beats an operator cue.
  assert(engine.update(f, 14, false, 1000).motor);    // Short x3: ON 1000-1199, 1400-1599, 1800-1999.
  f.haptic = Haptic::Removed;
  assert(engine.update(f, 14, false, 1250).motor);    // Removed (Long1, 300 ms) starts inside the command's OFF gap.
  f.haptic = Haptic::None;
  assert(engine.update(f, 14, false, 1549).motor);
  assert(!engine.update(f, 14, false, 1550).motor);   // Event done; the interrupted command does not resume (1550 was an ON window).

  // Role transitions do not cut a running command.
  assert(engine.update(f, 16, false, 3000).motor);    // Long x2: ON 3000-3599, 3800-4399.
  f.role = Role::Ghost; f.display = Display::Ghost;
  assert(!engine.update(f, 16, false, 3650).motor);   // Gap holds; the Ghost transition pulse is not started.
  assert(engine.update(f, 16, false, 3800).motor);
  assert(engine.update(f, 16, false, 4399).motor);
  assert(!engine.update(f, 16, false, 4400).motor);

  // Quiescent (setting/ready/ended) transitions do not cut a running command either.
  f = state(Role::Player, DeviceState::Activate, Phase::Active, Display::Player);
  engine.update(f, 0, false, 5000);                   // Player transition pulse (150 ms) plays and ends.
  assert(!engine.update(f, 0, false, 5500).motor);
  assert(engine.update(f, 17, false, 6000).motor);    // Long x3: ON 6000-6599, 6800-7399, 7600-8199.
  f.deviceState = DeviceState::Ready; f.phase = Phase::Ready; f.display = Display::Ready;
  assert(!engine.update(f, 17, false, 6700).motor);   // Ready did not cut it (its own pulse would be ON now).
  assert(engine.update(f, 17, false, 6900).motor);    // The command's second pulse continues.
  assert(!engine.update(f, 17, false, 8200).motor);

  // Commands work in any state. Invalid polls carry a forced vibe of 0 and must not create edges:
  // the same command afterwards stays silent, a changed one still plays.
  assert(engine.update(f, 12, false, 9000).motor);    // Ready state, command plays.
  assert(!engine.update(f, 12, false, 9200).motor);
  f.stateValid = false;
  assert(!engine.update(f, 0, false, 9500).motor);
  f.stateValid = true;
  assert(!engine.update(f, 12, false, 10000).motor);  // Same 12 after the blip: no replay.
  f.stateValid = false;
  engine.update(f, 0, false, 10500);
  f.stateValid = true;
  assert(engine.update(f, 13, false, 11000).motor);   // Changed during the blip: plays.
  assert(!engine.update(f, 13, false, 11600).motor);

  // OTA/reset consumes commands instead of replaying them afterwards.
  assert(!engine.update(f, 14, false, 12000, true).motor);
  assert(!engine.update(f, 14, false, 12100).motor);  // Released: 14 was consumed.
  assert(engine.update(f, 15, false, 12500).motor);   // A new edge after release plays normally.

  // A command already held at first sync or after an epoch change is consumed, not played.
  FeedbackEngine fresh;
  auto g = state(Role::Player, DeviceState::Activate, Phase::Active, Display::Player);
  assert(!fresh.update(g, 12, false, 0).motor);
  assert(!fresh.update(g, 12, false, 500).motor);
  g.stateEpoch++;                                     // Reconnect / identity replacement while 13 is held.
  assert(!fresh.update(g, 13, false, 1000).motor);
  assert(!fresh.update(g, 13, false, 1100).motor);
  assert(fresh.update(g, 12, false, 2000).motor);     // 13 -> 12 after the baseline is a real edge.

  // Mute silences game feedback too: proximity, chip events and transitions.
  FeedbackEngine muted;
  auto m = state(Role::Player, DeviceState::Activate, Phase::Active, Display::Player);
  muted.update(m, 0, true, 0);
  assert(muted.update(m, 3, true, 1000).motor);       // Same-room proximity pulse (t % 1000 < 100).
  assert(!muted.update(m, 10, true, 2000).motor);     // Muted inside a proximity ON window.
  m.haptic = Haptic::Removed;
  assert(!muted.update(m, 10, true, 2050).motor);     // Chip event swallowed.
  m.haptic = Haptic::None;
  m.deviceState = DeviceState::Ready; m.phase = Phase::Ready; m.display = Display::Ready;
  assert(!muted.update(m, 10, true, 2100).motor);     // Ready transition swallowed.
  m.deviceState = DeviceState::Activate; m.phase = Phase::Active; m.display = Display::Player;
  assert(!muted.update(m, 10, true, 2200).motor);     // Player transition swallowed.
  assert(muted.update(m, 3, true, 3000).motor);       // Released: proximity is back, no stale transition replays.
}
```

`int main()`에 `commandEdgesAndLevels();` 다음 줄로 `commandPrioritiesAndExceptions();` 추가.

- [ ] **Step 2: 실패 확인**

Run: 공통 명령(단일 테스트)
Expected: `Assertion failed: (!engine.update(f, 17, false, 6700).motor)` — Ready 전환이 명령을 끊고 자기 펄스를 시작했기 때문. (그보다 앞의 assert는 Task 2 구조로 이미 통과한다.)

- [ ] **Step 3: 구현**

`devices/iotglove/feedback.h`의 `update()`에서 **네 곳**을 고친다.

(a) 무효/억제 조기 반환 — `return out;` 직전에 소비 규칙 추가:

```cpp
    if (!state.stateValid || suppressed) {
      cancel();
      pendingGhostAck_ = false;
      known_ = state.stateValid;
      remember(state);
      // Suppressed (OTA/reset) polls consume a held command so it never plays late. Invalid polls
      // carry a forced vibe of 0 and leave lastVibe_ alone, so the next valid poll is not an edge.
      if (state.stateValid) { lastVibe_ = vibe; haveVibe_ = true; }
      return out;
    }
```

(b) baseline 블록 — 첫 동기화·epoch 변경에서만 소비:

```cpp
    if (baseline) {
      cancel();
      pendingGhostAck_ = false;
      // A command already held at first sync or after an identity/epoch change is consumed, not
      // played. A baseline caused only by a transient invalid poll keeps lastVibe_, so a command
      // that changed meanwhile still plays.
      if (!haveVibe_ || state.stateEpoch != epoch_) lastVibe_ = vibe;
      haveVibe_ = true;
    }
```

(c) 정지상태 `cancel()` — 명령 제외:

```cpp
    // Stop any preceding activity before a new setting/ready/end notification.
    // This is an edge, not a per-poll cancellation of the new notification.
    // Operator commands are not state feedback and outlive these transitions.
    if (source_ != Source::Command &&
        ((changed && quiescent(state)) ||
         (state.display == Display::Ended && display_ != Display::Ended))) cancel();
```

(d) 상태전환 시작 검사 — 명령 제외:

```cpp
    } else if (changed && !removedAcknowledged && source_ != Source::Event &&
               source_ != Source::Command) {
      // Consume transitions during explicit events or commands; never queue a stale replay.
      start(feedback_config::forState(state, settings_), Source::State, now);
    }
```

`haveVibe_` 멤버 주석을 `// False until the first valid poll; baseline consumption depends on it.`로 바꾼다.

- [ ] **Step 4: 통과 확인**

Run: 공통 명령(단일 테스트)
Expected: `PASS: ... operator vibe commands`

Run: 공통 명령(전체)
Expected: `Passed 15 firmware host test executables`

- [ ] **Step 5: 커밋**

```bash
cd /Users/byeongjun/workspace/New_HAS1
git add devices/iotglove/feedback.h devices/iotglove/tests/feedback_state_test.cpp
git commit -m "feat(iotglove): rank vibe commands below chip events, above state patterns; consume at sync and OTA" -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: 파서 허용 범위 0~17 (`wifi.cpp`) + 펌웨어 컴파일 검증

**Files:**
- Modify: `devices/iotglove/wifi.cpp:3` (include), `:98` (범위)

**Interfaces:**
- Consumes (Task 1): `feedback_config::kVibeCommandLast`
- Produces: 없음

이 파일은 Arduino 전용이라 호스트 테스트가 없다. 테스트 사이클은 **격리 스케치북 펌웨어 컴파일**이다.

- [ ] **Step 1: 격리 스케치북 준비 (없으면 생성)**

```bash
GB=/private/tmp/claude-501/-Users-byeongjun-workspace-New-HAS1/baab33f6-c845-4710-a3a8-40505d866d09/scratchpad/glovebook
if [ ! -f "$GB/libraries/iotglove-dependencies.json" ]; then
  GB=$(mktemp -d)/glovebook && mkdir -p "$GB" && printf 'directories:\n  data: %s/Library/Arduino15\n  downloads: %s/Library/Arduino15/staging\n  user: %s\n' "$HOME" "$HOME" "$GB" > "$GB/arduino-cli.yaml"
  python3 /Users/byeongjun/workspace/New_HAS1/devices/iotglove/tools/prepare_libraries.py --libraries-dir "$GB/libraries"
  arduino-cli --config-file "$GB/arduino-cli.yaml" lib install "ArduinoJson@7.4.3" "Adafruit NeoPixel@1.12.0"
fi
echo "GB=$GB"
```

Expected: 마지막 줄에 `GB=...` 경로. (이전 세션의 glovebook이 살아 있으면 그대로 재사용된다.)

- [ ] **Step 2: 수정 전 컴파일로 기준선 확인**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && python3 devices/iotglove/tools/compile.py ttgo --config-file "$GB/arduino-cli.yaml" --libraries-dir "$GB/libraries" 2>&1 | grep -E "^Sketch uses|error" 
```

Expected: `Sketch uses ... bytes (5x%) of program storage space.` 한 줄, `error` 없음 (Task 1~3 변경이 펌웨어 빌드를 깨지 않았음을 먼저 확인).

- [ ] **Step 3: 구현**

`devices/iotglove/wifi.cpp` 3행 `#include "chip_report.h"` 다음 줄에 추가:

```cpp
#include "feedback_config.h"
```

98~99행

```cpp
      !number(my["life_chip"], 0, 100, life) || !number(my["vibe"], 0, 3, vibe)) return false;
```

을 아래로 교체:

```cpp
      !number(my["life_chip"], 0, 100, life) ||
      // 0/1/3 proximity levels plus operator commands 10..17; anything else still rejects the snapshot.
      !number(my["vibe"], 0, feedback_config::kVibeCommandLast, vibe)) return false;
```

- [ ] **Step 4: 컴파일 검증 (ttgo + training)**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && for p in ttgo training; do python3 devices/iotglove/tools/compile.py $p --config-file "$GB/arduino-cli.yaml" --libraries-dir "$GB/libraries" 2>&1 | grep -E "^Compile|^Sketch uses|error"; done
```

Expected: `Compile ttgo: ...` / `Sketch uses ...` / `Compile training: ...` / `Sketch uses ...` — `error` 없음. 크기는 v7(1,089,429 B)에서 수백 B 이내 증가.

- [ ] **Step 5: 커밋**

```bash
cd /Users/byeongjun/workspace/New_HAS1
git add devices/iotglove/wifi.cpp
git commit -m "feat(iotglove): accept server vibe 0-17 so operator commands reach the feedback engine" -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: 문서 — 서버 계약과 README

**Files:**
- Modify: `devices/iotglove/docs/SERVER_CONTRACT.md:60` (표 행) + 절 추가
- Modify: `devices/iotglove/README.md:91`

**Interfaces:** 없음 (문서)

- [ ] **Step 1: SERVER_CONTRACT.md `vibe` 행 교체**

```
| `vibe` | 위치 계산 결과 수신 | 같은 방 **3**, 인접 방 **1**, 그 외 **0**. 주석의 같은 방=2보다 실제 함수 반환값을 기준으로 함 |
```

→

```
| `vibe` | 위치 계산 결과 수신 + 운영자 연출 명령 | 근접: 같은 방 **3**, 인접 방 **1**, 그 외 **0**. 연출 명령 **10~17**(v8, 아래 절). 허용 범위 0~17 정수, 그 밖은 스냅샷 거부 |
```

- [ ] **Step 2: SERVER_CONTRACT.md 연출 명령 절 추가**

`근거: [SET/증감 분류]... theme-family.js#L57).` 문단 **바로 뒤**(빈 줄 다음, `` `role=ghost` 보고는 `` 문단 앞)에 삽입:

```markdown
### vibe 연출 명령 (펌웨어 v8)

| `vibe` | 동작 | 유형 |
| --- | --- | --- |
| 10 | 음소거 — 유지되는 동안 모터 절대 OFF (근접·상태전환·칩 이벤트 전부 침묵) | 유지형 |
| 11 | 연속 ON — 유지되는 동안 모터 계속 ON | 유지형 |
| 12 / 13 / 14 | 짧은 진동 200ms × 1 / 2 / 3회, 간격 200ms (총 0.2 / 0.6 / 1.0초) | 값이 바뀐 순간 1회 |
| 15 / 16 / 17 | 긴 진동 600ms × 1 / 2 / 3회, 간격 200ms (총 0.6 / 1.4 / 2.2초) | 값이 바뀐 순간 1회 |
| 2, 4~9 | 무동작 (0과 동일) | — |

- `game_state`·`device_state`·`role`과 무관하게 동작한다. 예외는 OTA·리셋 중 모터 OFF만.
- 우선순위: 10 > 11 > 칩 제거/발견 진동 > 명령 진동 > 상태전환 진동 > 근접 진동.
- 12~17은 **값이 직전과 다른 값으로 바뀐 순간**에만 재생된다. 같은 값이 유지되면 재생하지 않는다.
  같은 명령을 반복하려면 다른 값(보통 0)을 거친다: `12 → 0 → 12`.
- **서버 계약**
  1. 각 값은 **최소 2초 유지**한다(글러브 폴링 1초, 한 번 놓쳐도 관측되게).
  2. 명령값(10~17)이 유지되는 동안 위치 기반 `vibe` 재계산이 이를 **덮어쓰지 않아야** 한다.
  3. 명령이 끝나면 근접 값(0/1/3)으로 되돌린다 — 명령값이 유지되는 동안 근접 진동은 멈춘다.
  4. 부팅·재연결 직후 첫 스냅샷에 들어 있는 명령값은 재생하지 않고 소비한다(OTA 재부팅 뒤 남아 있던 값이 울리지 않게).
  5. 11을 장시간 유지하면 모터가 계속 돌아 발열·배터리를 소모한다. 펌웨어 제한은 없다.
```

- [ ] **Step 3: README.md 91행 문단 끝에 문장 추가**

```
근접 진동 기본안은 같은 방(`vibe=3`) 1초당 100ms 두 번, 인접(`vibe=1`) 2초당 100ms 한 번이다. 칩/발각 진동이 우선하며 오래된 위치로는 울리지 않는다. 실제 방 경계에서 RSSI 필터와 진동 패턴을 조정한다.
```

→ 같은 줄 끝에 이어서:

```
 서버 `vibe` 10~17은 운영자 연출 명령이다: 10 음소거, 11 연속 ON, 12/13/14 짧은(200ms) 1~3회, 15/16/17 긴(600ms) 1~3회. 12~17은 값이 바뀌는 순간 1회만 울리고 상태·역할과 무관하게 동작한다(`docs/SERVER_CONTRACT.md`의 "vibe 연출 명령" 절 참고).
```

- [ ] **Step 4: 확인**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && grep -n "vibe 연출 명령\|10~17" devices/iotglove/docs/SERVER_CONTRACT.md devices/iotglove/README.md | head
```

Expected: SERVER_CONTRACT.md에 표 행·절 제목·README 문장까지 최소 3줄 매칭.

- [ ] **Step 5: 커밋**

```bash
cd /Users/byeongjun/workspace/New_HAS1
git add devices/iotglove/docs/SERVER_CONTRACT.md devices/iotglove/README.md
git commit -m "docs(iotglove): document server vibe commands 10-17 and the hold/recompute contract" -m "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 6: 최종 검증 · PR · 릴리즈 v8 · Notion 정리

**Files:** 변경 없음 (검증·배포)

**Interfaces:** 없음

- [ ] **Step 1: 전체 호스트 테스트**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && python3 devices/iotglove/tools/run_tests.py 2>&1 | tail -3
```

Expected: `Passed 15 firmware host test executables`

- [ ] **Step 2: 펌웨어 컴파일 (ttgo + training)**

Task 4 Step 4의 명령 그대로. Expected: 두 프로파일 모두 `Sketch uses ...`, `error` 없음.

- [ ] **Step 3: push + PR**

```bash
cd /Users/byeongjun/workspace/New_HAS1 && git push -u origin claude/iotglove-vibe-commands
```

```bash
cd /Users/byeongjun/workspace/New_HAS1 && gh pr create --base main --head claude/iotglove-vibe-commands --title "iotglove: 서버 vibe 연출 명령 10~17 (음소거·연속 ON·짧은/긴 진동 1~3회)" --body-file docs/superpowers/specs/2026-09-19-iotglove-vibe-commands-design.md
```

Expected: PR URL 출력. (본문은 스펙 문서 그대로. 필요하면 `gh pr edit`로 요약 추가. PR 본문 끝에 `🤖 Generated with [Claude Code](https://claude.com/claude-code)` 줄이 있어야 하므로 `--body-file` 대신 스펙 내용 + 그 줄을 붙인 임시 파일을 쓴다.)

- [ ] **Step 4: 릴리즈 (Deploy Firmware, 브랜치에서)**

허용 규칙 `Bash(gh workflow run *)`에 맞게 **단일 명령**으로 실행한다(`cd`나 `&&`로 묶지 않는다):

```bash
gh workflow run deploy-firmware.yml -R Fuzzyline-HAS2/New_HAS1 --ref claude/iotglove-vibe-commands -f device=iotglove -f update_partition=false
```

Expected: 출력 없음(접수). 약 2분 뒤:

```bash
cd /Users/byeongjun/workspace/New_HAS1 && id=$(gh run list -R Fuzzyline-HAS2/New_HAS1 --workflow deploy-firmware.yml --branch claude/iotglove-vibe-commands -L 1 --json databaseId --jq '.[0].databaseId') && gh run watch "$id" -R Fuzzyline-HAS2/New_HAS1 --exit-status -i 10 >/dev/null && git fetch -q origin claude/iotglove-vibe-commands && git --no-pager log origin/claude/iotglove-vibe-commands --oneline -2 && gh release download iotglove -R Fuzzyline-HAS2/New_HAS1 -p version.txt -O - && git merge -q --ff-only origin/claude/iotglove-vibe-commands
```

Expected: `Firmware v8` 커밋이 원격 최상단, 릴리즈 `version.txt` = `8`, 로컬 fast-forward.

- [ ] **Step 5: Notion 작업 카드 정리 (사용자 확인 후)**

사용자에게 "Notion 「IOT 글러브 연출 진동 기능 만들기」에 아래 요약을 기록해도 될까요?"라고 **먼저 확인**한다. 승인 시 `notion-update-page`로 카드 본문에 기록:
- 동작 표(SERVER_CONTRACT.md의 "vibe 연출 명령" 절과 동일)
- 서버 계약 1~5
- 펌웨어 버전 v8, PR 링크, 스펙 경로
- 진행상황 속성은 사용자가 바꾸도록 두고 건드리지 않는다

- [ ] **Step 6: 사용자에게 보고**

v8 릴리즈 완료, OTA는 서버에서 `device_state=github`(사용자 실행), 현장 확인 항목(12 한 번·15 한 번·10 후 칩 제거 시 침묵) 제시. PR 머지는 사용자 결정.

---

## Self-Review 결과

- **Spec coverage**: §2 동작 표 → Task 1·2·3 / §2-1 엣지 → Task 2 / §2-2 서버 계약 → Task 5 / §3-1 우선순위 → Task 3 / §3-2 예외 → Task 3 / §4 파일 → Task 1~4 / §5 테스트 → Task 1~3 (+Task 4 컴파일) / §6 배포 → Task 6 / §7 문서 → Task 5·6 / §8 서버 확인 → Task 5 계약 항목으로 전달. 누락 없음.
- **Placeholder scan**: TBD/TODO 없음. 모든 코드 단계에 실제 코드.
- **Type consistency**: `Schedule{onMs,gapMs,count,total}` · `pulses(uint32_t,uint32_t,uint32_t)` · `commandSchedule(uint8_t,const Settings&)` · `isCommand(uint8_t)` · `kVibeMute/kVibeOn/kVibeCommandFirst/kVibeCommandLast` · `Settings.commandShortMs/commandLongMs/commandGapMs` · `Source::Command` · `lastVibe_` · `haveVibe_` · `start(const Schedule&, Source, uint32_t)` — Task 1~4에서 동일 이름·타입 사용.
