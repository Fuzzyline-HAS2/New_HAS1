// =================================================================================
// contrib.ino
// ---------------------------------------------------------------------------------
// 스타터 단계의 게이지 기여도 기록 — "누가 얼만큼 채웠는지"를 세션 단위로 서버에 남긴다.
//
// 세션 = 카드가 리더에 올라와 role=="player"로 확인된 순간부터, 떼어지거나 스타터 단계를
// 벗어날 때까지. 한 세션이 서버 이벤트 로그 한 행이 된다. 한 사람이 여러 번 올렸다 떼면
// 행도 여러 개이고, 합산은 서버가 한다.
//
// 칸 수는 화면 표시용 displayedGaugeNeoCnt가 아니라 목표값(encoderValue/starterEncoderUnit)을
// 쓴다 — 표시는 칸당 최대 100ms 애니메이션 지연이 있어서, 빠르게 돌리고 곧바로 카드를 떼면
// 아직 표시되지 않은 칸이 누락된다. 세션의 시작값과 끝값이 둘 다 목표값 기준이라 뺄셈은
// 자기일관적이고, 방치 감소로 표시가 거꾸로 따라가는 중이어도 어긋나지 않는다.
//
// 전송은 기존 has2wifi.Situation()을 그대로 쓴다 (부활기가 이미 같은 API로 이벤트를 남긴다).
//   GET has2.php?request=Situation&table=generator_gauge&key=<발전기>&value=<GxPx>:<delta>
// =================================================================================

// 현재 게이지 목표 칸 수. StarterActivate()의 gaugeNeoCnt 계산과 같은 식을 쓰며,
// 세션 시작/종료 시점에도 같은 기준으로 칸 수를 읽기 위해 한 곳에 모아둔다.
int StarterGaugeCnt()
{
    int cnt = encoderValue / starterEncoderUnit;
    if (cnt > NumPixels[GAUGE]) cnt = NumPixels[GAUGE];
    if (cnt < 0) cnt = 0;   // encoderValue는 GameTimerFunc가 0에서 잡아주지만 방어적으로
    return cnt;
}

// 세션 시작 — 플레이어 코드와 그 시점의 칸 수를 기억한다.
void ContribBegin(const String &user, int cnt)
{
    starterContribUser = user;
    starterContribStartCnt = cnt;
    Serial.println("[Contrib] begin " + user + " @" + String(cnt));
}

// 세션 종료 — 순증(delta)이 1칸 이상이면 서버에 한 행을 남긴다.
// 세션이 열려 있지 않으면 아무 것도 하지 않으므로 호출부에서 따로 검사할 필요가 없다.
//
// has2wifi.Situation()은 블로킹 HTTP GET이다. 이 함수가 불리는 세 순간(카드 뗀 직후,
// 수리 완료 처리 직전, 스타터 이탈 다음 프레임)은 모두 엔코더가 멈춰 있거나 이미 블로킹
// 오디오가 들어 있는 loop() 컨텍스트다. DataChanged() 콜백 안에서는 절대 부르지 않는다 —
// BatteryFinish()가 ptrCurrentMode 대입으로 피해 둔 재진입과 같은 문제가 생긴다.
// 엔코더 펄스는 PCNT 하드웨어가 세므로 이 정지 동안 유실되지 않는다.
void ContribEnd(int cnt)
{
    if (starterContribUser.length() == 0) return;   // 열린 세션 없음

    // 전송 성공 여부와 무관하게 세션은 여기서 닫는다 (재시도하지 않는다) —
    // 실패를 이유로 세션을 열어두면 다음 종료 때 남의 구간까지 얹혀 더 크게 틀어진다.
    String user = starterContribUser;
    int startCnt = starterContribStartCnt;
    starterContribUser = "";

    int delta = cnt - startCnt;
    String line = "[Contrib] " + user + " " + String(startCnt) + "->" + String(cnt) +
                  " " + (delta >= 0 ? "+" : "") + String(delta);

    // 카드만 올리고 안 돌렸거나 방치 감소가 더 큰 세션 — 남길 게 없다.
    if (delta < 1){
        Serial.println(line + " skip");
        return;
    }

    BREADCRUMB("ContribEnd:send");
    bool ok = has2wifi.Situation(user + ":" + String(delta), "generator_gauge");
    Serial.println(line + (ok ? " OK" : " FAIL"));
}

// loop()에서 ptrCurrentMode() 바로 앞에 호출된다.
//
// 호출 위치가 중요하다 — 한 프레임 안에서 모드가 바뀔 수 있는 곳은 둘이다:
// ptrCurrentMode() 자신과, TimerRun()이 부르는 has2wifi.Loop(DataChanged).
// ptrCurrentMode() 앞에 두면 N번째 프레임 끝의 DataChanged가 바꾼 모드를 N+1 프레임 머리에서,
// 새 모드 함수가 한 번도 돌기 전에 정리한다. TimerRun() 뒤에 두면 폴링 GET 바로 뒤에
// 이 블로킹 GET이 같은 프레임에 연달아 붙는다.
//
// SettingFunc()/ReadyFunc()에서 ContribEnd를 직접 부르지 않는 이유도 같다 — 그 둘은
// DataChanged() 콜백 안에서 실행되므로 블로킹 Situation()을 넣으면 재진입이 재현된다.
// tagger 인터럽트, setting/ready 리셋, 서버가 직접 세트한 repaired 등 스타터 밖에서
// 일어나는 모든 이탈을 여기 한 곳에서 잡는다.
void ContribLoop()
{
    if (starterContribUser.length() == 0) return;   // 열린 세션 없음
    if (ptrCurrentMode == StarterActivate) return;  // 아직 스타터 진행 중

    ContribEnd(starterContribLastCnt);
    // 카드가 리더에 그대로 얹혀 있어도 스타터로 복귀했을 때 "새 태그"로 다시 인식되어
    // 새 세션이 열리도록 되돌린다.
    starterLastTagState = false;
}
