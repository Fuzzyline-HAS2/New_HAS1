#include "HAS1_revival_machine.h"

void TimerInit()
{
  wifi_timer_id = wifi_timer.setInterval(2000, WifiTimerFunc);
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
