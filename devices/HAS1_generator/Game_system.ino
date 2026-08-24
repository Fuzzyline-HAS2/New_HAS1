// =================================================================================
// Game_system.ino
// ---------------------------------------------------------------------------------
// 발전기 수리의 마지막 단계인 "스타터" 미니게임 루프.
// 배터리팩이 가득 충전되면(BatteryFinish) ptrCurrentMode가 StarterActivate로 바뀌고,
// 이후 loop()마다 이 함수가 호출된다.
//
// 진행 방식: 플레이어가 RFID 리더에 본인 카드를 올려둔 상태를 유지하면서 손잡이(엔코더)를 돌리면
// encoderValue가 증가하고, starterEncoderUnit 단위로 게이지가 한 칸씩 찬다.
// 카드를 떼거나 플레이어가 아닌(revival 등) 카드가 올라오면 엔코더 카운팅을 멈춰(EncoderDetach)
// 부정 진행을 막는다. 게이지가 가득 차면 StartFinish()를 호출해 수리 완료 처리로 넘어간다.
// =================================================================================
void StarterActivate(){
    // RFID 체크는 200ms마다만 수행 (블로킹으로 인한 루프 지연 방지)
    static bool tagOnReader = false;    // 이번 체크에서 리더 위에 태그가 감지됐는지
    static bool lastTagState = false;   // 직전 체크에서의 감지 상태 (새로 올라온 태그인지 판단용)
    static bool isPlayerTagged = false; // 현재 리더 위 태그가 role=="player"인지 여부
    static unsigned long lastRfidCheck = 0;
    if (millis() - lastRfidCheck >= 200){
        lastRfidCheck = millis();
        // RfidPresenceCheck()가 근접 Dead Zone 대응을 위해 근/원거리 Gain을 자동으로 오가며 확인한다
        // (rfid.ino 참고). 감지에 성공한 Gain은 내부에 유지되어 뒤이은 ntag2xx_ReadPage에도 그대로 쓰인다.
        tagOnReader = RfidPresenceCheck();
        if (tagOnReader) {
            if (!lastTagState) { // 새 태그가 올라왔을 때만 role 확인
                uint8_t data[32];
                if (nfc[MAINPN532].ntag2xx_ReadPage(7, data)){
                    String tagUser = "";
                    for(int i = 0; i < 4; i++) tagUser += (char)data[i];
                    has2wifi.Receive(tagUser); // 서버에 조회해 tag["role"]을 채움
                    isPlayerTagged = ((String)(const char*)tag["role"] == "player");
                    Serial.println(isPlayerTagged ? "Starter: Player OK" : "Starter: Revival Blocked");
                } else {
                    isPlayerTagged = false;
                }
            }
            // 태그가 계속 얹혀 있는 동안(lastTagState==true)은 재조회하지 않고
            // 직전에 판정한 isPlayerTagged 값을 그대로 유지한다 (통신 절약).
        }
        lastTagState = tagOnReader;
    }

    // 게이지 목표 칸 수(gaugeNeoCnt)는 encoderValue/starterEncoderUnit으로 즉시 계산되지만,
    // 화면에 실제로 보여주는 칸 수(displayedGaugeNeoCnt)는 목표치로 한 번에 점프하지 않고
    // 최소 GAUGE_STEP_INTERVAL_MS마다 1칸씩만 쫓아가도록 애니메이션한다.
    // starterEncoderUnit을 작게 잡으면(예: 1000) 손잡이를 살짝만 돌려도 그 사이 loop 판독
    // 한 번에 여러 칸이 한꺼번에 넘어가버려("한 번에 2~3칸씩") 화면이 끊겨 보이는데, 목표치와
    // 표시 칸 수를 분리해두면 실제로는 여러 칸이 한 번에 찼더라도 화면에는 한 칸씩 순차적으로
    // 이어져 보인다.
    // displayedGaugeNeoCnt는 함수-지역 static이 아니라 전역(HAS1_generator.h) — BatteryFinish()가
    // 새 충전 사이클마다 -1로 리셋해줘야, 이전 라운드에 다 찼던 값이 새 라운드까지 남아있지 않는다.
    const unsigned long GAUGE_STEP_INTERVAL_MS = 35; // 작을수록 더 즉각적, 클수록 더 부드러움
    static unsigned long lastGaugeStepTime = 0;
    static unsigned long lastGaugeRefresh = 0;
    int gaugeNeoCnt = encoderValue / starterEncoderUnit;
    if (gaugeNeoCnt > NumPixels[GAUGE]) gaugeNeoCnt = NumPixels[GAUGE];

    bool needRender = false;
    if (displayedGaugeNeoCnt < 0){
        displayedGaugeNeoCnt = gaugeNeoCnt;
        needRender = true;
    }
    else if (displayedGaugeNeoCnt != gaugeNeoCnt && millis() - lastGaugeStepTime >= GAUGE_STEP_INTERVAL_MS){
        lastGaugeStepTime = millis();
        displayedGaugeNeoCnt += (displayedGaugeNeoCnt < gaugeNeoCnt) ? 1 : -1;
        needRender = true;
    }
    // 변화가 없어도 500ms마다 재전송(깨진 프레임 자동 복구용)
    if (needRender || millis() - lastGaugeRefresh >= 500){
        lastGaugeRefresh = millis();
        EncoderNeopixelOn(displayedGaugeNeoCnt); // GAUGE 스트립에 진행률 표시
    }

    // 태그가 없거나 player가 아니면 엔코더 카운팅을 멈추고 즉시 리턴 (부정 진행 방지).
    // 이 상태에서도 위의 게이지 표시/RFID 체크는 계속 수행된다.
    if (!tagOnReader || !isPlayerTagged){
        EncoderDetach();
        return;
    }

    EncoderAttach(); // 정상 조건이면 엔코더 카운팅 재개(이미 카운팅 중이면 아무 동작 없음)

    // 디버그 출력은 1초에 한 번이면 충분
    static unsigned long lastRawPrint = 0;
    if (millis() - lastRawPrint >= 1000){
        lastRawPrint = millis();
        Serial.println("raw: " + String(encoderValue));
    }

    // 게이지가 가득 찼으면(모든 칸 점등) 수리 완료 처리로 전환.
    // displayedGaugeNeoCnt(화면에 실제로 다 찬 시점) 기준으로 판정해, 애니메이션이 목표치를
    // 미처 다 따라잡기도 전에 완료 처리가 먼저 튀어나오지 않게 한다.
    if(displayedGaugeNeoCnt >= NumPixels[GAUGE]){
        EncoderDetach();
        // SendCmd("page pgFixed");  // (Nextion 시절 잔재 — 현재 미사용)
        StartFinish();                              // 서버에 repaired 알림 및 다음 상태 전환
        BlinkTimer.deleteTimer(blinkTimerId);
        NeoLightColor(STARTER, color[BLUE]);
        GameTimer.deleteTimer(gameTimerId);        //게임 타이머 종료
        BlinkTimer.deleteTimer(blinkTimerId);
        NeoLightColor(CIRCUIT, color[BLUE]);
    }
}
