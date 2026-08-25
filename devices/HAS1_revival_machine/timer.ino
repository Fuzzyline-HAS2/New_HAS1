#include "HAS1_revival_machine.h"

static unsigned long wifi_poll_interval_ms = WIFI_POLL_INTERVAL_DEFAULT_MS;

void TimerInit()
{
  wifi_timer_id = wifi_timer.setInterval(wifi_poll_interval_ms, WifiTimerFunc);
}

// SimpleTimer는 기존 타이머의 주기를 바꾸는 API가 없어서, 타이머를 지우고 같은 콜백으로
// 다시 등록하는 방식으로 주기를 바꾼다. game_state가 activate로 바뀔 때만 짧게(300ms)
// 좁혀서 device_state="open" 반영 지연을 줄이고, 벗어나면 기본(2초)으로 되돌려 서버 부하를 아낀다.
void SetWifiPollInterval(unsigned long ms)
{
  if (ms == wifi_poll_interval_ms) return;
  wifi_poll_interval_ms = ms;
  wifi_timer.deleteTimer(wifi_timer_id);
  wifi_timer_id = wifi_timer.setInterval(wifi_poll_interval_ms, WifiTimerFunc);
}
/**
 * @brief 타이머 동작
 */
void TimerRun()
{
  BleAdvertiserMaintain();
  rfid_timer.run();
  nsec_tag_timer.run();
  wifi_timer.run();
}

/**
 * @brief RFID가 연속적으로 찍히지 않게 하기위해 플래그를 줌
 */
void RfidTagTimerFunc()
{
  rfid_tag = false;
}

void WifiTimerFunc()
{
  has2wifi.Loop(DataChange);
}

void NsecTagTimerFailFunc()
{
  Serial.println("태그 실패");
  nsec_tag_num = 0;
  nsec_tag_bool = false;
}

void NsecTagTimerSuccessFunc()
{
  Serial.println("태그 성공 후 2초");
  nsec_tag_num = 0;
}
