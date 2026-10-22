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
// 전송은 발전기 단일 네트워크 작업자의 고정 크기 큐에 등록한다.
//   GET has2.php?request=Situation&table=generator_gauge&key=<발전기>&value=<GxPx>:<delta>
// =================================================================================

// Queue rejection is not transmission failure: keep up to four unsent events.
// Once accepted by the network worker an event is never automatically repeated.
struct PendingContribution { char user[16]; int delta; uint32_t generation; };
static PendingContribution pendingContributions[4];
static unsigned pendingContributionCount=0;
static uint32_t starterContribGeneration=0;
static void ContribDrainPending() {
    if(!pendingContributionCount)return;
    PendingContribution &event=pendingContributions[0];
    const bool stale=event.generation!=GeneratorNetworkGeneration();
    if(!stale&&!GeneratorNetworkContribution(event.user,event.delta))return;
    if(stale)DiagnosticPrintf("[NET] unsent contribution discarded after game generation changed\r\n");
    for(unsigned i=1;i<pendingContributionCount;++i)pendingContributions[i-1]=pendingContributions[i];
    --pendingContributionCount;
}

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
    starterContribGeneration = GeneratorNetworkGeneration();
    Serial.println("[Contrib] begin " + user + " @" + String(cnt));
}

// 세션 종료 — 순증(delta)이 1칸 이상이면 서버에 한 행을 남긴다.
// 세션이 열려 있지 않으면 아무 것도 하지 않으므로 호출부에서 따로 검사할 필요가 없다.
//
// 큐 접수 전 거절만 최대 네 건 보관한다. 전송에 착수한 이벤트는 응답이 없어도
// 자동 재전송하지 않는다. 기존 Situation API로 전송하며, 서버의 게임 세대
// 검증이 없어 이미 전송한 이벤트의 지연 반영까지 취소할 수는 없다.
void ContribEnd(int cnt)
{
    if (starterContribUser.length() == 0) return;   // 열린 세션 없음

    // 전송 성공 여부와 무관하게 세션은 여기서 닫는다 (재시도하지 않는다) —
    // 실패를 이유로 세션을 열어두면 다음 종료 때 남의 구간까지 얹혀 더 크게 틀어진다.
    String user = starterContribUser;
    int startCnt = starterContribStartCnt;
    starterContribUser = "";
    if(starterContribGeneration!=GeneratorNetworkGeneration()) {
        DiagnosticPrintf("[NET] active contribution discarded after game generation changed\r\n");
        return;
    }

    int delta = cnt - startCnt;
    String line = "[Contrib] " + user + " " + String(startCnt) + "->" + String(cnt) +
                  " " + (delta >= 0 ? "+" : "") + String(delta);

    // 카드만 올리고 안 돌렸거나 방치 감소가 더 큰 세션 — 남길 게 없다.
    if (delta < 1){
        Serial.println(line + " skip");
        return;
    }

    BREADCRUMB("ContribEnd:send");
    if (GeneratorNetworkContribution(user.c_str(), delta)) Serial.println(line + " queued");
    else if(pendingContributionCount<4&&user.length()<sizeof(pendingContributions[0].user)) {
        PendingContribution &event=pendingContributions[pendingContributionCount++];
        snprintf(event.user,sizeof(event.user),"%s",user.c_str());event.delta=delta;event.generation=GeneratorNetworkGeneration();
        DiagnosticPrintf("[NET] unsent contribution retained in bounded pending queue\r\n");
    } else DiagnosticPrintf("[NET] contribution pending queue full; event rejected, no synchronous fallback\r\n");
}

// 서버 상태 변경은 NetworkApplySnapshot에서 인코더 초기화 전에 세션을 닫는다.
// 이 함수는 로컬 모드 이탈을 직전 프레임의 게이지 값으로 정리한다.
// 새 게임 세대의 시작 전 열려 있던 세션/미전송 이벤트는 다음 게임에 합산하지 않는다.
void ContribLoop()
{
    ContribDrainPending();
    if (starterContribUser.length() == 0) return;   // 열린 세션 없음
    if (ptrCurrentMode == StarterActivate) return;  // 아직 스타터 진행 중

    ContribEnd(starterContribLastCnt);
    // 카드가 리더에 그대로 얹혀 있어도 스타터로 복귀했을 때 "새 태그"로 다시 인식되어
    // 새 세션이 열리도록 되돌린다.
    starterLastTagState = false;
}
