//****************************************GAME SYSTEM (STATE MACHINE)****************************************************************
//
//  상태 전환 원칙:
//  - 전환은 ChangeGameState() 단 하나를 통해서만.
//  - Exit action (현재 상태 정리)은 ChangeGameState() 내 exit switch에만.
//  - Entry action (LED·타이머·플래그 초기화)도 ChangeGameState() 내 entry switch에만.
//  - 상태 함수는 자기 로직만 — 다른 상태의 진입/정리 조건을 몰라도 된다.
//  - ChangeGameState() 포함 모든 상태 함수에서 delay() 금지.

// role 조회 대기 중인 태그 (ChangeGameState(GAME_ACTIVATE) entry에서 초기화)
static String pendingTagUser = "";

// ── 상태 전환 ──────────────────────────────────────────────────────────────────────────
void ChangeGameState(GameState next) {
    Log("GAME", String(GameStateName(gameState)) + " -> " + String(GameStateName(next)));

    // Exit Action: 현재 상태 정리
    switch (gameState) {
        case GAME_PUZZLE:
        case GAME_PAUSED:
            vibrationOff();
            EncoderDisable();
            // GameTimer.stop();  // Step 3: SimpleTimer 연동 시 활성화
            break;
        case GAME_CORRECT_ANIM:
        case GAME_WRONG_ANIM:
        case GAME_ITEM_FAIL_ANIM:
        case GAME_TAGGER_ANIM:
            blinkCnt = 0;
            blinkR.periodMs = 50;   // 기본 주기 복원
            break;
        default:
            break;
    }

    // Entry Action: 다음 상태 초기화
    switch (next) {
        case GAME_SETTING:
            NeoSetAll(WHITE);
            boxOpen();
            answerCnt = 0;
            break;
        case GAME_READY:
            NeoSetAll(RED);
            boxClose();
            answerCnt = 0;
            break;
        case GAME_ACTIVATE:
            NeoSetAll(YELLOW);
            boxClose();
            answerCnt = 0;
            EncoderReset();
            pendingTagUser = "";  // role 조회 대기 상태 초기화
            break;
        case GAME_PUZZLE:
            NeoSetAll(BLUE);
            lastButtonPressed = isEncoderButtonPressed();  // 진입 순간 버튼 상태 동기화
            rfidLastSeenTime  = millis();                  // 태그 이탈 판정 기준 초기화
            EncoderEnable();
            GameEventSend("device_state", "solving");      // 서버에 퍼즐 진행 중 상태 보고
            break;
        case GAME_PAUSED:
            NeoSetAll(YELLOW);
            pauseStartTime = millis();
            GameEventSend("device_state", "activate");
            break;
        case GAME_CORRECT_ANIM:
        case GAME_WRONG_ANIM:
        case GAME_ITEM_FAIL_ANIM:
            blinkCnt = 0;
            ledOn    = false;
            blinkR.periodMs = 250;  // 점멸 주기 250ms (ON 250ms + OFF 250ms = 0.5s/cycle)
            blinkR.lastRun  = 0;    // 진입 즉시 첫 점멸
            break;
        case GAME_BOX_OPENING:
            NeoSetAll(GREEN);
            boxOpen();
            break;
        case GAME_BOX_OPEN:
            NeoSet(NEO_INNER, YELLOW);  // 내부 태그 유도 (하드웨어 미연결 시 무시됨)
            break;
        case GAME_USED:
            NeoSetAll(BLUE);
            boxOpen();
            break;
        case GAME_DONE:
            NeoSetAll(BLUE);
            boxOpen();
            break;
        case GAME_TAGGER:
            NeoSetAll(PURPLE);
            pendingTagUser = "";  // role 조회 대기 상태 초기화 (모터는 관여하지 않음)
            answerCnt = 0;        // 퍼즐 도중 태거가 끼어든 경우 — 태거 종료 후 퍼즐은 처음부터 다시 시작
            break;
        case GAME_TAGGER_ANIM:
            blinkCnt = 0;
            ledOn    = false;
            blinkR.periodMs = 250;  // 점멸 주기 250ms (ON 250ms + OFF 250ms = 0.5s/cycle)
            blinkR.lastRun  = 0;    // 진입 즉시 첫 점멸
            break;
    }

    gameState = next;
}

// ── 메인 디스패처 ──────────────────────────────────────────────────────────────────────
void GameUpdate() {
    switch (gameState) {
        case GAME_SETTING:        SettingState();       break;
        case GAME_READY:          ReadyState();         break;
        case GAME_ACTIVATE:       ActivateState();      break;
        case GAME_PUZZLE:         PuzzleState();        break;
        case GAME_PAUSED:         PausedState();        break;
        case GAME_CORRECT_ANIM:   CorrectAnimState();   break;
        case GAME_WRONG_ANIM:     WrongAnimState();     break;
        case GAME_BOX_OPENING:    BoxOpeningState();    break;
        case GAME_BOX_OPEN:       BoxOpenState();       break;
        case GAME_ITEM_FAIL_ANIM: ItemFailAnimState();  break;
        case GAME_USED:                                 break;  // 서버 이벤트 대기 (DataChanged)
        case GAME_DONE:                                 break;  // 종료 상태, 입력 차단
        case GAME_TAGGER:         TaggerState();        break;
        case GAME_TAGGER_ANIM:    TaggerAnimState();    break;
    }
}

// 엔코더 위치와 정답 거리에 따라 진동 세기 설정 — 게임 로직, HAL 호출만 함
static void setVibrationByProximity(int answer, long encValue) {
    int diff  = abs(answer - (int)encValue);
    int aRange = modeValue[RANGE][ANSWER_RANGE];
    int vRange = modeValue[RANGE][VIBRATION_RANGE];
    int grade;
    if      (diff < aRange + vRange * 0) grade = 0;
    else if (diff < aRange + vRange * 1) grade = 1;
    else if (diff < aRange + vRange * 2) grade = 2;
    else if (diff < aRange + vRange * 3) grade = 3;
    else                                 grade = 4;
    vibrationOn(modeValue[VIBESTREGNTH][grade]);
}

// ArduinoJson | 0 은 서버가 숫자를 string으로 내릴 때 0을 반환.
// 이 헬퍼는 int형·string형 모두 파싱한다.
static int jsonInt(JsonVariant v) {
    if (v.is<int>())         return v.as<int>();
    const char* s = v.as<const char*>();
    return s ? atoi(s) : 0;
}

// ── 퍼즐 정답·카운트·리셋 시간 갱신 ──────────────────────────────────────────────────
void UpdatePuzzleAnswers() {
    int cnt = jsonInt(myDoc["puzzle_count"]);
    if (cnt > 0) modeValue[RANGE][ANSWER_CNT] = cnt;

    const char* keys[] = {
        "puzzle_answer_1","puzzle_answer_2","puzzle_answer_3",
        "puzzle_answer_4","puzzle_answer_5"
    };
    for (int i = 0; i < 5; i++) {
        int v = jsonInt(myDoc[keys[i]]);
        if (v != 0) modeValue[ANSWER][i] = v;
    }

    int rt = jsonInt(myDoc["puzzle_reset_time"]);
    if (rt != 0) puzzleResetTime = (unsigned long)rt;
}

// ── LED 밝기 갱신 ─────────────────────────────────────────────────────────────────
void UpdateBrightness() {
    int b = myDoc["brightness"] | 0;
    if (b != 0) NeoSetBrightness(b);
}

// ── 서버 이벤트 처리 ────────────────────────────────────────────────────────────────
// myDoc: xQueueReceive 후 Core1에서 역직렬화한 최신 서버 상태
void DataChanged() {
    static String prevGameState   = "";
    static String prevDeviceState = "";
    // game_state: 서버 씬 전환 (setting / ready / activate)
    String gs = myDoc["game_state"] | "";
    if (gs != "" && gs != prevGameState) {
        if      (gs == "setting")  ChangeGameState(GAME_SETTING);
        else if (gs == "ready")    ChangeGameState(GAME_READY);
        else if (gs == "activate") ChangeGameState(GAME_ACTIVATE);
        prevGameState = gs;
    }
    // activate는 중복 수신이어도 항상 닫기
    if (gs == "activate") boxClose();

    // device_state: 개별 기기 명령 (언제든지 수신 가능)
    String ds = myDoc["device_state"] | "";
    if (ds != "" && ds != prevDeviceState) {
        if      (ds == "activate")    ChangeGameState(GAME_ACTIVATE);
        else if (ds == "open")        ChangeGameState(GAME_BOX_OPENING);
        else if (ds == "close")       boxClose();
        else if (ds == "used")        ChangeGameState(GAME_USED);
        else if (ds == "repaired_all" ||
                 ds == "player_win"   ||
                 ds == "player_lose") ChangeGameState(GAME_DONE);
        else if (ds == "tagger")      ChangeGameState(GAME_TAGGER);
        else if (ds == "github")      otaRequested = true;  // Core0으로 OTA 트리거 전달
        // "solving" : HAS1에서 미사용 (Core0/1 분리로 WiFi 타이머 불필요) — 수신 시 무시
        prevDeviceState = ds;
    }
    // activate는 중복 수신(이미 ACTIVATE 상태)이어도 항상 닫기
    if (ds == "activate") boxClose();

    // 설정값 갱신 — 상태 전환 여부와 무관하게 항상 적용
    UpdatePuzzleAnswers();
    UpdateBrightness();
}

// ── 퍼즐 완료 ──────────────────────────────────────────────────────────────────────────
void PuzzleSolved() {
    Log("GAME", "quiz solved");
    Mp3PlayLargeFolder(1, 1);
    ChangeGameState(GAME_BOX_OPENING);  // boxOpen()은 GAME_BOX_OPENING entry action에서 호출
}

//---------------------------------------- GAME_SETTING ----------------------------------------
// 서버 game_state=setting 상태. 박스 열림·흰불. DataChanged()가 READY/ACTIVATE로 전환.
void SettingState() {
    // 서버 이벤트 대기 — DataChanged()가 처리함
}

//---------------------------------------- GAME_READY ----------------------------------------
// 서버 game_state=ready 상태. 태그 시 경고만, 퍼즐 시작 안 됨.
void ReadyState() {
    if (!RfidTagPresent()) return;  // 캐시 조회 (RfidHalUpdate 200ms 갱신)

    uint8_t data[32];
    if (!RfidReadTag(data)) return;  // 200ms마다 1회 소비 가능

    Log("GAME", "tag in READY — game not activated yet");
}

//---------------------------------------- GAME_ACTIVATE ----------------------------------------
// 외부 RFID 태그 대기. role 조회는 Core0 Queue 경유 비동기 처리.
void ActivateState() {
    // (A) role 조회 응답 대기 중 — 결과 폴링
    if (pendingTagUser != "") {
        String role = WifiPollPlayerRole();  // 논블로킹, Core0 미완료 시 ""
        if (role == "player") {
            Log("GAME", "puzzle start by " + pendingTagUser);
            pendingTagUser = "";
            ChangeGameState(GAME_PUZZLE);
        } else if (role != "") {
            Log("GAME", "non-player ignored (" + role + ")");
            pendingTagUser = "";
        }
        return;
    }

    // (B) 신규 태그 감지 → role 조회 요청 투입
    if (!RfidTagPresent()) return;
    uint8_t data[32];
    if (!RfidReadTag(data)) return;

    String tagUser = "";
    for (int i = 0; i < 4; i++) tagUser += (char)data[i];
    Log("GAME", "tag: " + tagUser);

    if (tagUser == "MMMM") { Log("GAME", "admin card -> restart"); ESP.restart(); return; }

    pendingTagUser = tagUser;
    WifiRequestPlayer(tagUser.c_str());  // roleRequestQueue 투입, 즉시 리턴
    Log("GAME", "role request sent for: " + tagUser);
}

//---------------------------------------- GAME_TAGGER ----------------------------------------
// device_state=tagger 상태. 보라색 상시 점등, 외부 태그(player/ghost) 시 오디오+전체 점멸 연출만
// 수행한다 (모터 개폐는 관여하지 않음). role 조회는 ActivateState와 동일하게 Core0 Queue 경유.
// 같은 태그를 오래 얹어 두면 RfidHalUpdate(200ms)가 계속 재읽기해서 아래 role 조회가 반복
// 트리거되는데, 그때마다 사운드가 울리면 시끄럽다 — 마지막 연출 이후 이 시간(ms) 안에는
// 다시 태그가 감지돼도 사운드/점멸을 재실행하지 않는다.
#define TAGGER_EFFECT_COOLDOWN_MS 3000

void TaggerState() {
    static unsigned long lastEffectMs = 0 - TAGGER_EFFECT_COOLDOWN_MS; // 최초 태그는 바로 재생되도록

    // (A) role 조회 응답 대기 중 — 결과 폴링
    if (pendingTagUser != "") {
        String role = WifiPollPlayerRole();  // 논블로킹, Core0 미완료 시 ""
        if (role == "player" || role == "ghost") {
            String taggedUser = pendingTagUser;
            pendingTagUser = "";
            if (millis() - lastEffectMs < TAGGER_EFFECT_COOLDOWN_MS) {
                Log("GAME", "tagger effect skipped (cooldown): " + taggedUser);
            } else {
                Log("GAME", "tagger effect by " + taggedUser + " (" + role + ")");
                lastEffectMs = millis();
                Mp3PlayLargeFolder(1, 6);          // 오디오 먼저 트리거
                ChangeGameState(GAME_TAGGER_ANIM); // 그 다음 보라색 점멸 연출
            }
        } else if (role != "") {
            Log("GAME", "tagger: non-target role ignored (" + role + ")");
            pendingTagUser = "";
        }
        return;
    }

    // (B) 신규 태그 감지 → role 조회 요청 투입
    if (!RfidTagPresent()) return;
    uint8_t data[32];
    if (!RfidReadTag(data)) return;

    String tagUser = "";
    for (int i = 0; i < 4; i++) tagUser += (char)data[i];
    Log("GAME", "tagger tag: " + tagUser);

    pendingTagUser = tagUser;
    WifiRequestPlayer(tagUser.c_str());  // roleRequestQueue 투입, 즉시 리턴
}

//---------------------------------------- GAME_PUZZLE ----------------------------------------
// 엔코더로 정답 위치 탐색, 버튼으로 확정. 태그 이탈 시 PAUSED.
void PuzzleState() {
    // 태그 유지 확인 — RfidTagPresent()는 캐시 조회 (하드웨어 접근 없음, loop마다 안전)
    if (RfidTagPresent()) rfidLastSeenTime = millis();
    if (millis() - rfidLastSeenTime > RFID_PUZZLE_TIMEOUT) {
        ChangeGameState(GAME_PAUSED);
        return;
    }

    int currentAnswer = modeValue[ANSWER][answerCnt];

    if (currentAnswer == -1) {
        Log("GAME", "answer -1: server force-solve");
        PuzzleSolved();
        return;
    }

    long encValue = readEncoderValue();
    NeoEncoderUpdate(encValue);
    setVibrationByProximity(currentAnswer, encValue);

    bool pressed      = isEncoderButtonPressed();
    bool justPressed  = pressed && !lastButtonPressed;
    lastButtonPressed = pressed;
    if (!justPressed) return;

    int diff = abs(currentAnswer - (int)readEncoderValue());
    if (diff < modeValue[RANGE][ANSWER_RANGE]) {
        answerCnt++;
        Log("GAME", "answer " + String(answerCnt) + "/" + String(modeValue[RANGE][ANSWER_CNT]) + " correct");
        ChangeGameState(GAME_CORRECT_ANIM);
    } else {
        Log("GAME", "wrong (enc=" + String(readEncoderValue()) + " target=" + String(currentAnswer) + ")");
        ChangeGameState(GAME_WRONG_ANIM);
    }
}

//---------------------------------------- GAME_PAUSED ----------------------------------------
// 태그 이탈 일시정지. 재태그 → 퍼즐 재개, puzzleResetTime 초과 → ACTIVATE 복귀.
void PausedState() {
    if (millis() - pauseStartTime > puzzleResetTime) {
        Log("GAME", "pause " + String(puzzleResetTime / 1000) + "s timeout -> ACTIVATE");
        ChangeGameState(GAME_ACTIVATE);
        return;
    }

    if (!RfidTagPresent()) return;  // 캐시 조회

    ChangeGameState(GAME_PUZZLE);
}

//---------------------------------------- GAME_CORRECT_ANIM ----------------------------------------
// 정답 연출. blinkR.due(250ms)마다 LED 토글, 5회 점멸 후 다음 상태로 전환.
void CorrectAnimState() {
    if (!blinkR.due) return;
    ledOn = !ledOn;
    NeoSet(NEO_ENCODER, ledOn ? GREEN : BLACK);
    if (ledOn) blinkCnt++;
    if (blinkCnt < 5) return;

    bool nextIsTerminator = (answerCnt < modeValue[RANGE][ANSWER_CNT])
                         && (modeValue[ANSWER][answerCnt] == -1);
    if (answerCnt >= modeValue[RANGE][ANSWER_CNT] || nextIsTerminator)
        PuzzleSolved();
    else
        ChangeGameState(GAME_PUZZLE);
}

//---------------------------------------- GAME_WRONG_ANIM ----------------------------------------
// 오답 연출. blinkR.due(250ms)마다 LED 토글, 5회 점멸 후 PUZZLE 복귀.
void WrongAnimState() {
    if (!blinkR.due) return;
    ledOn = !ledOn;
    NeoSet(NEO_ENCODER, ledOn ? RED : BLACK);
    if (ledOn) blinkCnt++;
    if (blinkCnt < 5) return;
    ChangeGameState(GAME_PUZZLE);
}

//---------------------------------------- GAME_BOX_OPENING ----------------------------------------
// 모터가 박스를 여는 중. 열림 감지 즉시 GAME_USED로 전환 (내부 RFID 없음 — 열림 = 수령 완료).
// GameEventSend를 ChangeGameState entry가 아닌 여기서 호출 — 물리 개방 시에만 서버 보고.
// (서버 "used" 에코백 → DataChanged → GAME_USED 재진입 시 중복 전송 방지)
void BoxOpeningState() {
    if (!isBoxOpened()) return;
    GameEventSend("device_state", "used");
    ChangeGameState(GAME_USED);
}

//---------------------------------------- GAME_BOX_OPEN ----------------------------------------
// 내부 RFID 미설치. 퍼즐/서버 "open" 경로는 GAME_BOX_OPENING → GAME_USED로 직행.
// 이 상태는 현재 하드웨어에서 도달하지 않음 (미래 내부 RFID 추가 시 활용).
void BoxOpenState() {}

//---------------------------------------- GAME_ITEM_FAIL_ANIM ----------------------------------------
// 배터리팩 초과 오류 연출. blinkR.due(250ms)마다 LED 토글, 8회 점멸 후 BOX_OPEN 복귀.
// NEO_INNER 미연결 시 NeoSet이 내부적으로 무시.
void ItemFailAnimState() {
    if (!blinkR.due) return;
    ledOn = !ledOn;
    NeoSet(NEO_INNER, ledOn ? RED : BLACK);
    if (ledOn) blinkCnt++;
    if (blinkCnt < 8) return;
    ChangeGameState(GAME_BOX_OPEN);
}

//---------------------------------------- GAME_TAGGER_ANIM ----------------------------------------
// 태그 연출. blinkR.due(250ms)마다 전체 네오픽셀을 보라색<->꺼짐으로 토글, 2회 점멸 후 GAME_TAGGER 복귀.
void TaggerAnimState() {
    if (!blinkR.due) return;
    ledOn = !ledOn;
    NeoSetAll(ledOn ? PURPLE : BLACK);
    if (ledOn) blinkCnt++;
    if (blinkCnt < 2) return;
    ChangeGameState(GAME_TAGGER);
}
