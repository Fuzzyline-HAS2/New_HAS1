#include "HAS1_duct.h"

/**
 * @brief 덕트 사용시 동작
 */
void DuctTag(String tag_player)
{
    if (duct_available)
    {
        tag_player_name = tag_player;
        use_duct_num++;
        CooltimeCalculation();
        DuctOpen();
        TagPlayerSend();
    }
    else
    {
        CooltimeMp3();
    }
}

void DuctOpen(bool switch_push)
{
    // 봉쇄("이로운 효과") 중 내부 스위치: 도어만 열고 상태·네오픽셀·서버 보고는 건드리지 않는다.
    // duct_available 을 내리면 CooltimeTimerFunc 가 tagger_mode 로 조기 return 하므로 쿨타임이
    // 진행되지 않아, 봉쇄가 풀릴 때까지 덕트가 빨간 잠금 상태로 고착된다(프리즈).
    // 도어가 열려 있는 동안 덕트킬은 EMCHECK_PIN 기준으로 그대로 동작한다.
    if (tagger_mode && switch_push)
    {
        if (!can_exit_on_tagger)
        {
            switch_available = false;   // 연타로 오디오가 계속 재생되지 않도록 동일한 방식으로 쿨다운
            TaggerSwitchBlocked();
            // 색은 계속 보라색 그대로 둔다 - back을 받아 ExitTaggerMode가 호출될 때
            // ApplyCurrentNeopixel()이 알맞은 색으로 되돌려준다.
            duct_close_timer_id = duct_close_timer.setTimeout(4000, TaggerSwitchClose);
            return;
        }

        switch_available = false;
        Mp3PlayLargeFolder(1, 2);
        digitalWrite(RELAY_PIN, HIGH);
        duct_close_timer_id = duct_close_timer.setTimeout(4000, TaggerSwitchClose);
        return;
    }

    if (duct_available || switch_push)
    {
        if (cooltime_timer.isEnabled(cooltime_timer_id))
        {
            cooltime_timer.deleteTimer(cooltime_timer_id);
        }
        Mp3PlayLargeFolder(1, 2);
        switch_available = false;
        duct_available = false;
        pixels_line.lightColor(line_red);
        pixels_switch.lightColor(red);
        pixels_round.lightColor(red);
        digitalWrite(RELAY_PIN, HIGH);
        duct_close_timer_id = duct_close_timer.setTimeout(4000, DuctClose);
    }
}

void DuctClose()
{
    digitalWrite(RELAY_PIN, LOW);
    pixels_line.lightColor(line_red);
    pixels_switch.lightColor(yellow);
    switch_available = true;
    current_time = 0;
    cool_time_neo_bool = true;
    if (!cooltime_timer.isEnabled(cooltime_timer_id))
    {
        cooltime_timer_id = cooltime_timer.setInterval(1000, CooltimeTimerFunc);
    }
    has2wifi.Send((String)(const char *)my["device_name"], "device_state", "lock");
}

/**
 * @brief 봉쇄 중 내부 스위치로 열린 도어를 닫는다.
 *        duct_available / current_time / cooltime / 네오픽셀 / 서버 상태는 손대지 않는다.
 */
void TaggerSwitchClose()
{
    digitalWrite(RELAY_PIN, LOW);
    switch_available = true;
}

/**
 * @brief can_exit_on_tagger=false일 때 봉쇄 중 내부 스위치를 눌러도 문을 열지 않고
 *        거부 피드백만 준다: 스위치 네오픽셀 보라색 + 사용 불가 안내 음성.
 *        (전용 트랙이 없어 기존 (1,1) placeholder를 재사용한다.)
 */
void TaggerSwitchBlocked()
{
    pixels_switch.lightColor(purple);
    Mp3PlayLargeFolder(1, 1);
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

void TagPlayerSend()
{
    has2wifi.Send((String)(const char *)my["device_name"], "tag_player", tag_player_name);
}

void DuctKill()
{
    // 가장 최근 태그한 플레이어 정보를 DB에서 가져옴
    String kill_player = (String)(const char *)my["tag_player"];

    has2wifi.Receive(kill_player);

    if (kill_player.startsWith("G"))
    {
        if ((String)(const char *)tag["role"] == "player")
        {
            Serial.println("Duct Kill!");
            Mp3PlayLargeFolder(4, 3);

            // 덕트킬은 tagger("이로운 효과")와 완전히 동일하게 동작한다.
            // UI를 먼저 즉시 반영(EnterTaggerMode)한 뒤 서버에 device_state=tagger로 알린다.
            // (서버가 "back"을 보내면 ExitTaggerMode()로 그대로 복귀한다.)
            EnterTaggerMode();
            has2wifi.Send((String)(const char *)my["device_name"], "device_state", "tagger");
        }
    }
}

/**
 * @brief 봉쇄(tagger_mode) 중 생존자/ghost/revival이 태그했을 때의 피드백.
 *        보라색 점멸(3회, non-blocking) + 사용 불가 안내 음성.
 *        (1,1)은 전용 트랙이 생기기 전까지 임시로 사용하는 자리표시자(placeholder)다.
 */
void TaggerModeTagBlocked()
{
    Mp3PlayLargeFolder(1, 1);

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

    mmmm_prev_duct_available     = duct_available;
    mmmm_prev_cooltime_running   = cooltime_timer.isEnabled(cooltime_timer_id);
    mmmm_prev_current_time       = current_time;
    mmmm_prev_cool_time_neo_bool = cool_time_neo_bool;

    if (mmmm_prev_cooltime_running)
        cooltime_timer.deleteTimer(cooltime_timer_id);

    switch_available = false;
    duct_available   = false;
    Mp3PlayLargeFolder(1, 2);
    pixels_line.lightColor(line_red);
    pixels_switch.lightColor(red);
    pixels_round.lightColor(red);
    digitalWrite(RELAY_PIN, HIGH);
    duct_close_timer_id = duct_close_timer.setTimeout(4000, MmmmClose);
}

void MmmmClose()
{
    digitalWrite(RELAY_PIN, LOW);
    mmmm_open        = false;
    switch_available = true;

    if (mmmm_prev_duct_available)
    {
        duct_available    = true;
        cool_time_neo_bool = false;
        pixels_line.lightColor(line_yellow);
        pixels_round.lightColor(yellow);
        pixels_switch.lightColor(yellow);
    }
    else
    {
        duct_available    = false;
        current_time      = mmmm_prev_current_time;
        cool_time_neo_bool = mmmm_prev_cool_time_neo_bool;
        pixels_line.clear();
        pixels_line.lightColor(line_red, CooltimeBarPixels());
        pixels_switch.lightColor(yellow);
        // 잠긴 상태로 남길 때는 카운트다운이 반드시 돌아야 한다. 이전 실행 여부로 판단하면
        // (스위치/RFID 오픈 4초 중 MMMM 태그처럼) 타이머가 없던 시점을 기억해 재가동을 건너뛰고,
        // duct_available 이 false 에 고착돼 game_state 가 바뀔 때까지 덕트가 잠긴다.
        // DuctClose 와 동일하게 "지금 돌고 있지 않으면 켠다"로 판단한다.
        if (!cooltime_timer.isEnabled(cooltime_timer_id))
            cooltime_timer_id = cooltime_timer.setInterval(1000, CooltimeTimerFunc);
        has2wifi.Send((String)(const char *)my["device_name"], "device_state", "lock");
    }
}
