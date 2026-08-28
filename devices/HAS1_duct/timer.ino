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

void CooltimeTimerFunc()
{
    if(tagger_mode) return;   // "이로운 효과"(덕트킬 포함) 동결 중 쿨타임 일시정지 (current_time/cooltime 보존)
    if(current_time >= cooltime){
        pixels_line.lightColor(line_yellow);
        pixels_round.lightColor(yellow);
        pixels_switch.lightColor(yellow);

        has2wifi.Send((String)(const char*)my["device_name"], "device_state", "activate");
        current_time = 0;
        duct_available = true;
        cool_time_neo_bool = false;
        cooltime_timer.deleteTimer(cooltime_timer_id);
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
  has2wifi.Loop(DataChange);
}