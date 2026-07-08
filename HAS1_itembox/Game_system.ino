//****************************************GAME SYSTEM (STATE MACHINE)****************************************************************
//
//  GAME_WAIT_TAG ──(NFC 태그)──> GAME_PUZZLE ──(정답 3개 완료)──> GAME_SOLVED (박스 열림) ──(태그로 박스 열기)──> GAME_WAIT_TAG 
//                                    ⇅ (태그 이탈 0.5초 / 재태그)
//                               GAME_PAUSED
//
//  상태 전환은 이 파일 안에서만 일어난다. loop()는 GameUpdate() 하나만 호출하면 됨.

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
// NFC 카드가 태그되기를 기다린다. 태그되면 퍼즐 모드 진입
void WaitTagState()
{
    if (millis() - lastRfidCheckTime < RFID_CHECK_INTERVAL) return;
    lastRfidCheckTime = millis();

    // 직전 동작(박스 닫기)에 쓴 카드가 아직 올려져 있으면, 뗄 때까지 새 게임을 시작하지 않는다
    if (waitTagRelease)
    {
        if (RfidTagPresent()) { tagAbsentSince = millis(); return; }
        if (millis() - tagAbsentSince < TAG_RELEASE_TIME) return;
        waitTagRelease = false;             // 카드 뗀 것 확인 → 이제 새 태그를 받는다
        Serial.println("Tag released, ready for new game");
        return;
    }

    uint8_t data[32];
    if (!RfidReadTag(data)) return;         // 태그 없음 → 다음 loop에서 다시 확인

    String tagUser = CheckingPlayers(data);

    answerCnt = 0;
    encoder.clearCount();                   // 엔코더 0 위치에서 퍼즐 시작
    lastButtonPressed = isEncoderButtonPressed(); // 태그 순간 버튼이 눌려있어도 오작동 없게
    rfidLastSeenTime = millis();            // 태그 이탈 감지 기준 시각 초기화
    lightColor(pixels[NEO_PN532], color[BLUE]); // 퍼즐 진행 중 육각 네오픽셀은 파란색
    EncoderNeopixelOn();
    gameState = GAME_PUZZLE;
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
    if (millis() - rfidLastSeenTime > RFID_PUZZLE_TIMEOUT)  // 태그 이탈 → 일시정지
    {
        Serial.println("Puzzle Paused: RFID 태그 없음");
        vibrationOff();
        AllNeoOn(YELLOW);
        pauseStartTime = millis();  // 30초 방치 리셋의 기준 시각
        gameState = GAME_PAUSED;    // answerCnt는 유지 → 재태그 시 이어서 진행
        return;
    }

    int currentAnswer = modeValue[ANSWER][answerCnt];

    EncoderNeopixelOn();                    // 현재 엔코더 위치를 네오픽셀로 표시
    EncoderVibrationStrength(currentAnswer);// 정답에 가까울수록 진동 강하게

    // 버튼 에지 검출
    bool pressed = isEncoderButtonPressed();
    bool justPressed = pressed && !lastButtonPressed;
    lastButtonPressed = pressed;
    if (!justPressed) return;

    // 정답 판정: 정답 위치 ± ANSWER_RANGE 안이면 정답 (이전 프로젝트와 동일한 기준)
    int differenceValue = abs(currentAnswer - (int)readEncoderValue());
    if (differenceValue < modeValue[RANGE][ANSWER_RANGE])
    {
        Serial.println("Correct Answer " + String(answerCnt + 1));
        NeoBlink(NEO_ENCODER, GREEN, 5, 250);   // 정답 연출 (delay 블로킹 2.5초 - 연출 중 입력 무시 의도)
        rfidLastSeenTime = millis();            // 블로킹 연출 동안 태그 감지가 없었으므로 오탐 방지

        answerCnt++;
        if (answerCnt >= modeValue[RANGE][ANSWER_CNT])  // 모든 정답을 맞췄으면
            PuzzleSolved();
    }
    else
    {
        Serial.println("Wrong Answer");
        NeoBlink(NEO_ENCODER, RED, 5, 250);     // 오답 연출
        rfidLastSeenTime = millis();            // 블로킹 연출 동안 태그 감지가 없었으므로 오탐 방지
    }
}

// 퍼즐 완료: 성공 연출 후 박스 열기 시작 (열림 완료는 boxUpdate()가 처리)
void PuzzleSolved()
{
    Serial.println("QUIZ SUCCEED");
    vibrationOff();
    AllNeoOn(GREEN);
    Mp3PlayLargeFolder(1, 1);   // 성공음
    boxOpen();                  // 4초 뒤 boxUpdate()가 자동 정지
    waitTagRelease = true;      // 퍼즐 내내 올려둔 카드가 아직 리더에 있음 → 뗐다 다시 태그해야 닫기 동작
    tagAbsentSince = millis();
    gameState = GAME_SOLVED;
}

//---------------------------------------- GAME_PAUSED ----------------------------------------
// 태그 이탈로 일시정지. 재태그하면 풀던 문제부터 이어서, 30초 방치되면 퍼즐 리셋
void PausedState()
{
    if (millis() - pauseStartTime > PUZZLE_RESET_TIME)  // 30초 방치 → 처음부터
    {
        Serial.println("Puzzle Reset: 일시정지 " + String(PUZZLE_RESET_TIME / 1000) + "초 경과");
        answerCnt = 0;
        AllNeoOn(WHITE);        // 대기 상태 표시로 복귀
        gameState = GAME_WAIT_TAG;
        return;
    }

    if (millis() - lastRfidCheckTime < RFID_CHECK_INTERVAL) return;  // 여기도 200ms 주기로만 확인
    lastRfidCheckTime = millis();
    if (!RfidTagPresent()) return;

    Serial.println("Puzzle Resumed");
    rfidLastSeenTime = millis();
    lastButtonPressed = isEncoderButtonPressed(); // 재개 순간 버튼 오작동 방지
    lightColor(pixels[NEO_PN532], color[BLUE]);   // 재개: 육각 네오픽셀 노란색 → 파란색 복귀
    EncoderNeopixelOn();                          // 엔코더 링도 퍼즐 표시로 복귀
    gameState = GAME_PUZZLE;
}

//---------------------------------------- GAME_SOLVED ----------------------------------------
// 박스 열림 완료 후 대기. 카드를 다시 태그하면 박스를 닫고 처음 상태로 복귀
void SolvedState()
{
    if (boxState != BOX_IDLE) return;       // 박스가 아직 움직이는 중이면 대기

    if (millis() - lastRfidCheckTime < RFID_CHECK_INTERVAL) return;
    lastRfidCheckTime = millis();

    // 퍼즐 푸는 동안 올려둔 카드가 그대로 있으면 닫기 태그로 오인하지 않도록, 먼저 떼기를 기다린다
    if (waitTagRelease)
    {
        if (RfidTagPresent()) { tagAbsentSince = millis(); return; }
        if (millis() - tagAbsentSince < TAG_RELEASE_TIME) return;
        waitTagRelease = false;             // 카드 뗀 것 확인 → 이제 새 태그가 닫기 명령
        Serial.println("Tag released, tag again to close box");
        return;
    }

    uint8_t data[32];
    if (!RfidReadTag(data)) return;

    CheckingPlayers(data);
    Serial.println("Box closing, game reset");
    boxClose();
    AllNeoOn(WHITE);
    waitTagRelease = true;                  // 닫기에 쓴 카드도 뗐다 태그해야 다음 게임 시작
    tagAbsentSince = millis();
    gameState = GAME_WAIT_TAG;
}
