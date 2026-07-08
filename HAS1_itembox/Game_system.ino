//****************************************GAME SYSTEM (STATE MACHINE)****************************************************************
//
//  GAME_WAIT_TAG ──(NFC 태그)──> GAME_PUZZLE ──(정답 3개 완료)──> GAME_SOLVED (박스 열림) ──(태그로 박스 닫기)──> GAME_WAIT_TAG
//                                    ⇅ (태그 이탈 / 재태그)
//                               GAME_PAUSED
//
//  구조 원칙:
//  - 상태 전환은 반드시 ChangeGameState()를 통해서만 한다.
//  - 각 상태의 "진입 시 해야 할 일"(LED 색, 타이머/플래그 초기화)은 ChangeGameState() 안에만 있다.
//    → 상태 함수들은 서로의 진입 조건을 몰라도 되고, 자기 상태의 로직만 담당한다.
//  - loop()는 GameUpdate() 하나만 호출하면 됨.

// 상태 전환 전용 함수. 새 상태의 진입 처리를 한곳에서 담당한다
void ChangeGameState(GameState next)
{
    Log("GAME", String(GameStateName(gameState)) + " -> " + String(GameStateName(next)));
    switch (next) {
        case GAME_WAIT_TAG:                 // 대기: 흰불, 직전에 쓴 카드는 뗐다 태그해야 인정
            AllNeoOn(WHITE);
            waitTagRelease = true;
            tagAbsentSince = millis();
            break;

        case GAME_PUZZLE:                   // 퍼즐: 육각 파랑 + 링 퍼즐 표시, 버튼/태그이탈 기준 초기화
            lastButtonPressed = isEncoderButtonPressed();  // 진입 순간 버튼이 눌려있어도 오작동 없게
            rfidLastSeenTime = millis();                   // 태그 이탈 판정 기준 시각 초기화
            lightColor(pixels[NEO_PN532], color[BLUE]);
            EncoderNeopixelOn();
            break;

        case GAME_PAUSED:                   // 일시정지: 진동 끄고 노란불, 방치 리셋 타이머 시작
            vibrationOff();
            AllNeoOn(YELLOW);
            pauseStartTime = millis();
            break;

        case GAME_SOLVED:                   // 해결: 진동 끄고 초록불, 올려둔 카드는 뗐다 태그해야 닫기
            vibrationOff();
            AllNeoOn(GREEN);
            waitTagRelease = true;
            tagAbsentSince = millis();
            break;
    }
    gameState = next;
}

// 카드가 리더에서 떨어졌는지 확인. TAG_RELEASE_TIME 연속 미감지면 뗌 확정(waitTagRelease 해제) 후 true
bool TagReleaseConfirmed()
{
    if (RfidTagPresent()) { tagAbsentSince = millis(); return false; }
    if (millis() - tagAbsentSince < TAG_RELEASE_TIME) return false;
    waitTagRelease = false;
    return true;
}

// loop()에서 매번 호출. 현재 상태에 맞는 처리 함수로 분기한다
void GameUpdate()
{
    switch (gameState) {
        case GAME_WAIT_TAG: WaitTagState(); break;
        case GAME_PUZZLE:   PuzzleState();  break;
        case GAME_PAUSED:   PausedState();  break;
        case GAME_SOLVED:   SolvedState();  break;
    }
}

//---------------------------------------- GAME_WAIT_TAG ----------------------------------------
// NFC 카드가 태그되기를 기다린다. 태그되면 새 게임을 초기화하고 퍼즐 모드 진입
void WaitTagState()
{
    if (millis() - lastRfidCheckTime < RFID_CHECK_INTERVAL) return;
    lastRfidCheckTime = millis();

    if (waitTagRelease)
    {
        if (TagReleaseConfirmed()) Log("GAME", "tag released, ready for new game");
        return;
    }

    uint8_t data[32];
    if (!RfidReadTag(data)) return;         // 태그 없음 → 다음 loop에서 다시 확인

    String tagUser = CheckingPlayers(data);
    Log("GAME", "puzzle start by " + tagUser);

    answerCnt = 0;                          // 새 게임 초기화 (재개가 아닌 여기서만)
    encoder.clearCount();                   // 엔코더 0 위치에서 퍼즐 시작
    ChangeGameState(GAME_PUZZLE);
}

//---------------------------------------- GAME_PUZZLE ----------------------------------------
// 엔코더를 돌려 정답 위치를 찾고(진동이 힌트), 버튼으로 확정. 정답 3개를 모두 맞추면 박스 열림
void PuzzleState()
{
    // 태그 유지 확인: 카드가 리더 위에 있어야 퍼즐이 진행된다
    // PN532 통신은 한 번에 수십 ms 걸려서 매 loop 확인하면 네오픽셀/엔코더 반응이 둔해짐 → 200ms에 한 번만
    if (millis() - lastRfidCheckTime >= RFID_CHECK_INTERVAL)
    {
        lastRfidCheckTime = millis();
        if (RfidTagPresent())
            rfidLastSeenTime = millis();
    }
    if (millis() - rfidLastSeenTime > RFID_PUZZLE_TIMEOUT)  // 태그 이탈 → 일시정지 (answerCnt 유지)
    {
        ChangeGameState(GAME_PAUSED);   // 전환 로그(PUZZLE -> PAUSED)가 남음
        return;
    }

    int currentAnswer = modeValue[ANSWER][answerCnt];

    EncoderNeopixelOn();                    // 현재 엔코더 위치를 네오픽셀로 표시
    EncoderVibrationStrength(currentAnswer);// 정답에 가까울수록 진동 강하게

    // 버튼 에지 검출
    // 엔코더가 박혔을 때를 대비하여
    bool pressed = isEncoderButtonPressed();
    bool justPressed = pressed && !lastButtonPressed;
    lastButtonPressed = pressed;
    if (!justPressed) return;

    // 정답 판정: 정답 위치 ± ANSWER_RANGE 안이면 정답 (3호점 아이템박스와 동일한 기준)
    int differenceValue = abs(currentAnswer - (int)readEncoderValue());
    if (differenceValue < modeValue[RANGE][ANSWER_RANGE])
    {
        Log("GAME", "answer " + String(answerCnt + 1) + "/" + String(modeValue[RANGE][ANSWER_CNT]) + " correct");
        NeoBlink(NEO_ENCODER, GREEN, 5, 250);   // 정답 연출 (delay 블로킹 2.5초 - 연출 중 입력 무시 의도)
        rfidLastSeenTime = millis();            // 블로킹 연출 동안 태그 감지가 없었으므로 오탐 방지

        answerCnt++;
        if (answerCnt >= modeValue[RANGE][ANSWER_CNT])  // 모든 정답을 맞췄으면
            PuzzleSolved();
    }
    else
    {
        Log("GAME", "wrong answer (enc " + String(readEncoderValue()) + ", target " + String(currentAnswer) + ")");
        NeoBlink(NEO_ENCODER, RED, 5, 250);     // 오답 연출
        rfidLastSeenTime = millis();            // 블로킹 연출 동안 태그 감지가 없었으므로 오탐 방지
    }
}

// 퍼즐 완료 이벤트: 성공음 + 박스 열기 시작 (열림 완료는 boxUpdate()가, 연출은 GAME_SOLVED 진입 처리가 담당)
void PuzzleSolved()
{
    Log("GAME", "quiz solved");
    Mp3PlayLargeFolder(1, 1);   // 성공음
    boxOpen();                  // 4초 뒤 boxUpdate()가 자동 정지
    ChangeGameState(GAME_SOLVED);
}

//---------------------------------------- GAME_PAUSED ----------------------------------------
// 태그 이탈로 일시정지. 재태그하면 풀던 문제부터 이어서, 30초 방치되면 퍼즐 리셋
void PausedState()
{
    if (millis() - pauseStartTime > PUZZLE_RESET_TIME)  // 방치 → 처음부터
    {
        Log("GAME", "pause " + String(PUZZLE_RESET_TIME / 1000) + "s timeout -> puzzle reset");
        ChangeGameState(GAME_WAIT_TAG);
        return;
    }

    if (millis() - lastRfidCheckTime < RFID_CHECK_INTERVAL) return;
    lastRfidCheckTime = millis();
    if (!RfidTagPresent()) return;

    ChangeGameState(GAME_PUZZLE);           // answerCnt 유지된 채 이어서 진행 (전환 로그: PAUSED -> PUZZLE)
}

//---------------------------------------- GAME_SOLVED ----------------------------------------
// 박스 열림 완료 후 대기. 카드를 뗐다가 다시 태그하면 박스를 닫고 처음 상태로 복귀
void SolvedState()
{
    if (boxState != BOX_IDLE) return;       // 박스가 아직 움직이는 중이면 대기

    if (millis() - lastRfidCheckTime < RFID_CHECK_INTERVAL) return;
    lastRfidCheckTime = millis();

    if (waitTagRelease)
    {
        if (TagReleaseConfirmed()) Log("GAME", "tag released, tag again to close box");
        return;
    }

    uint8_t data[32];
    if (!RfidReadTag(data)) return;

    CheckingPlayers(data);
    boxClose();                             // MOTOR 로그(box closing)가 남음                             // 마이크로 스위치가 눌리면 boxUpdate()가 자동 정지
    ChangeGameState(GAME_WAIT_TAG);
}
