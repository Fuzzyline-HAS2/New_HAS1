#include "HAS1_duct.h"
void TimerInit()
{
    wifi_timer_id = wifi_timer.setInterval(2000, WifiTimerFunc);
}

void TimerRun()
{
    cooltime_timer.run();
    duct_close_timer.run();
    wifi_timer.run();
    rfid_timer.run();
    tagger_blink_timer.run();
}

/**
 * @brief 쿨타임 완료 처리. 타이머 만료와 서버 activate(ServerActivate) 양쪽에서 공유한다.
 */
void CooltimeFinish(bool notify_server)
{
    // Send는 동기 HTTP 요청이다. 통신 전에 사용 가능 상태와 타이머를 확정한다.
    current_time = 0;
    duct_available = true;
    cool_time_neo_bool = false;
    cooltime_timer.deleteTimer(cooltime_timer_id);

    pixels_line.lightColor(line_yellow);
    pixels_round.lightColor(yellow);
    pixels_switch.lightColor(yellow);

    if (notify_server)
        has2wifi.Send((String)(const char*)my["device_name"], "device_state", "activate");
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

void RfidTagTimerFunc()
{
    rfid_tag = false;
}

void WifiTimerFunc()
{
  has2wifi.LoopFresh(DataChange);
}