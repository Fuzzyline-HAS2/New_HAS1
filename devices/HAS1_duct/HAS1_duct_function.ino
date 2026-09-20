#include "HAS1_duct.h"

/**
 * @brief 덕트 사용시 동작
 */
void DuctTag(String tag_player)
{
    if (duct_available)
    {
        // [진단 로그] tag_player가 정상적으로 서버에 전송되는 경로인지 확인용.
        Serial.print("[DuctTag] duct_available - recording tag_player="); Serial.println(tag_player);
        tag_player_name = tag_player;
        // 사용횟수 증가와 쿨타임 재계산은 실제로 문을 여는 DuctOpen()이 맡는다.
        // (내부 스위치 개방도 똑같이 1회로 세기 위함)
        DuctOpen();
        TagPlayerSend();
    }
    else
    {
        // [진단 로그] 쿨타임 중이라 tag_player가 갱신/전송되지 않고 무시된다 - 이게
        // "태그했는데 tag_player가 안 채워졌다"의 유력한 원인 중 하나다.
        Serial.print("[DuctTag] duct NOT available (cooltime) - tag_player NOT sent for "); Serial.println(tag_player);
        CooltimeMp3();
    }
}

/**
 * @brief 개방 안내. 밖에서 열면(태그·서버 관리 개방·MMMM) "당겨주십시오"(09/0712),
 *        안에서 스위치로 열면 "밀어주십시오" 계열(09/0719). 영어는 Mp3MakePhrase가 폴더 10으로 바꾼다.
 */
void OpenMp3(bool inside)
{
    Mp3PlayLargeFolder(9, inside ? 719 : 712);
}

void DuctOpen(bool switch_push)
{
    if (mmmm_open) return;
    // 봉쇄 중 내부 스위치는 문을 열지 않고 사용 불가 피드백만 준다.
    if (tagger_mode && switch_push)
    {
        TaggerSwitchBlocked();
        return;
    }

    // switch_push는 쿨타임 관문을 통과시키는 근거가 될 수 없다. 이전 조건(duct_available ||
    // switch_push)은 내부 스위치 호출이 항상 switch_push=true라서 쿨타임 중에도 무조건 열렸고,
    // 이어지는 DuctClose()가 current_time=0으로 쿨타임까지 리셋했다(현장 리포트).
    // 비상탈출은 EMNERGENCY_CHK_PIN(EmegencyPush) 별도 경로이므로 이 게이팅으로 갇히지 않는다.
    // 쿨타임 중 스위치 네오픽셀은 이미 빨간색으로 사용 불가를 표시한다(커밋 ff0dbfc).
    if (duct_available)
    {
        // 외부 태그든 내부 스위치든 실제로 열린 개방만 1회 사용으로 세고, 그 횟수로 이번
        // 쿨타임을 다시 계산한다. 예전에는 DuctTag()에만 있어서 내부 스위치로 열면 쿨타임
        // 증가 사다리가 전혀 오르지 않았고, 한 번도 태그 없이 스위치부터 누르면
        // cooltime 이 0 인 채로 잠겨 1초 만에 풀렸다(현장 리포트).
        // 관리자 개방(MmmmOpen)은 이 경로를 타지 않으므로 여전히 횟수에서 제외된다.
        use_duct_num++;
        CooltimeCalculation();
        if (cooltime_timer.isEnabled(cooltime_timer_id))
        {
            cooltime_timer.deleteTimer(cooltime_timer_id);
        }
        OpenMp3(switch_push);
        switch_available = false;
        duct_available = false;
        // 문이 닫힌 뒤 시작할 쿨타임을 준비한다.
        current_time = 0;
        cool_time_neo_bool = true;
        pixels_line.lightColor(line_red);
        pixels_switch.lightColor(red);
        pixels_round.lightColor(red);
        digitalWrite(RELAY_PIN, HIGH);
        duct_close_timer_id = duct_close_timer.setTimeout(4000, DuctClose);
    }
    else if (switch_push)
    {
        CooltimeMp3();
    }
}

void DuctClose()
{
    digitalWrite(RELAY_PIN, LOW);
    switch_available = true;
    if (!cooltime_timer.isEnabled(cooltime_timer_id))
    {
        cooltime_timer_id = cooltime_timer.setInterval(1000, CooltimeTimerFunc);
    }
    // 봉쇄 중에도 쿨타임은 준비하되, 타이머 함수에서 진행을 멈춘다.
    if (tagger_mode) return;
    pixels_line.lightColor(line_red);
    pixels_switch.lightColor(red);
    has2wifi.Send((String)(const char *)my["device_name"], "device_state", "lock");
}

/**
 * @brief 봉쇄 중 내부 스위치를 눌러도 문을 열지 않고
 *        거부 피드백만 준다: 스위치 네오픽셀 보라색 + 봉쇄 남은시간 안내.
 */
void TaggerSwitchBlocked()
{
    pixels_switch.lightColor(purple);
    TaggerRemainingMp3();
}

/**
 * @brief 남은 쿨타임을 네오픽셀 개수로 환산한다.
 *        cooltime 이 0이면(= 쿨타임 없이 잠긴 상태) 나눗셈을 건너뛴다.
 *        ESP32(Xtensa)는 정수 0 나누기에서 IntegerDivideByZero 예외로 패닉한다.
 */
int CooltimeBarPixels()
{
    if (cooltime <= 0) return NUMPIXELS_LINE;
    return NUMPIXELS_LINE * (cooltime - current_time) / cooltime;
}

/**
 * @brief 덕트 사용횟수에 따른 쿨타임 계산 함수
 */
void CooltimeCalculation()
{
    switch (use_duct_num)
    {
    case 1:
    case 2:
        cooltime = cooltime_set + cooltime_add * 0;
        break;
    case 3:
    case 4:
        cooltime = cooltime_set + cooltime_add * 1;
        break;
    case 5:
    case 6:
        cooltime = cooltime_set + cooltime_add * 2;
        break;
    case 7:
    case 8:
        cooltime = cooltime_set + cooltime_add * 3;
        break;
    case 9:
    case 10:
        cooltime = cooltime_set + cooltime_add * 4;
        break;
    default:
        break;
    }
}

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

void TagPlayerSend()
{
    has2wifi.Send((String)(const char *)my["device_name"], "tag_player", tag_player_name);
}

void DuctKill()
{
    Serial.println("Duct Kill!");
    Mp3PlayLargeFolder(4, 1);

    // 술래 태그와 문 열림 여부는 CardChecking에서 확인한다. 봉쇄 해제는 서버가 처리한다.
    EnterTaggerMode();
    has2wifi.Send((String)(const char *)my["device_name"], "device_state", "tagger");
}

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

void TaggerRemainingMp3()
{
    RemainingTimeMp3(4, 2, TaggerRemainingSeconds());
}

/**
 * @brief 봉쇄(tagger_mode) 중 생존자/ghost/revival이 태그했을 때의 피드백.
 *        보라색 점멸(3회, non-blocking) + 사용 불가 안내 음성.
 */
void TaggerModeTagBlocked()
{
    TaggerRemainingMp3();

    if (tagger_blink_active) return;   // 점멸 중 재태그는 무시 (타이머 중첩 방지)
    tagger_blink_active = true;
    tagger_blink_step = 0;
    tagger_blink_timer_id = tagger_blink_timer.setInterval(250, TaggerBlinkStep);
}

/**
 * @brief 점멸 1스텝(끄기/켜기 전환). 3회 점멸(=6스텝) 후 정지하고 보라색 고정 상태로 복귀한다.
 *        중간에 봉쇄가 먼저 풀리면(ExitTaggerMode) 점멸을 즉시 중단한다.
 */
void TaggerBlinkStep()
{
    if (!tagger_mode)
    {
        tagger_blink_timer.deleteTimer(tagger_blink_timer_id);
        tagger_blink_active = false;
        return;
    }

    if (tagger_blink_step % 2 == 0)
    {
        pixels_line.clear();  pixels_line.show();
        pixels_round.clear(); pixels_round.show();
    }
    else
    {
        pixels_line.lightColor(line_purple);
        pixels_round.lightColor(purple);
    }
    tagger_blink_step++;

    if (tagger_blink_step >= 6)   // 3회 점멸 완료
    {
        tagger_blink_timer.deleteTimer(tagger_blink_timer_id);
        tagger_blink_active = false;
        pixels_line.lightColor(line_purple);
        pixels_round.lightColor(purple);
    }
}

void MmmmOpen()
{
    if (mmmm_open) return;
    mmmm_open = true;

    // 일반 개방 중 관리자 태그가 들어오면 닫기 예약을 하나로 합친다.
    if (duct_close_timer.isEnabled(duct_close_timer_id))
        duct_close_timer.deleteTimer(duct_close_timer_id);

    mmmm_prev_duct_available     = duct_available;
    mmmm_prev_current_time       = current_time;
    mmmm_prev_cool_time_neo_bool = cool_time_neo_bool;

    if (cooltime_timer.isEnabled(cooltime_timer_id))
        cooltime_timer.deleteTimer(cooltime_timer_id);

    switch_available = false;
    duct_available   = false;
    OpenMp3(false);
    if (!tagger_mode)
    {
        pixels_line.lightColor(line_red);
        pixels_switch.lightColor(red);
        pixels_round.lightColor(red);
    }
    digitalWrite(RELAY_PIN, HIGH);
    duct_close_timer_id = duct_close_timer.setTimeout(4000, MmmmClose);
}

void MmmmClose()
{
    digitalWrite(RELAY_PIN, LOW);
    mmmm_open        = false;
    switch_available = true;
    duct_available = mmmm_prev_duct_available;
    current_time = mmmm_prev_current_time;
    cool_time_neo_bool = mmmm_prev_cool_time_neo_bool;

    // 일반 개방에서 넘어온 쿨타임도 여기서 시작한다. 관리자 개방만으로는 만들지 않는다.
    if (!duct_available && !cooltime_timer.isEnabled(cooltime_timer_id))
        cooltime_timer_id = cooltime_timer.setInterval(1000, CooltimeTimerFunc);

    if (tagger_mode) return;
    ApplyCurrentNeopixel();
    if (duct_available && game_state == activate)
        has2wifi.Send((String)(const char *)my["device_name"], "device_state", "activate");
    if (!duct_available)
    {
        pixels_line.clear();
        pixels_line.lightColor(line_red, CooltimeBarPixels());
        has2wifi.Send((String)(const char *)my["device_name"], "device_state", "lock");
    }
}
