#include "HAS1_altar.h"

void TimerInit()
{
  static const esp_task_wdt_config_t wdt_cfg = {
    .timeout_ms    = 30000,
    .idle_core_mask = 0,
    .trigger_panic  = true,
  };
  esp_task_wdt_reconfigure(&wdt_cfg);  // 이미 초기화된 WDT 설정만 변경
  esp_task_wdt_add(NULL);              // 현재 태스크(loop) 등록
  wifi_timer_id = wifi_timer.setInterval(2000, WifiTimerFunc);
}
/**
 * @brief 타이머 동작
 */
void TimerRun()
{
  esp_task_wdt_reset();
  g_loop_count++;
  BleAdvertiserMaintain();
  rfid_timer.run();
  nsec_tag_timer.run();
  wifi_timer.run();
  solenoid_timer.run();
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
  BREADCRUMB("WifiTimerFunc");
  has2wifi.Loop(DataChange);
  CrashReportSend((const char *)my["device_name"]);
  CrashNvsFlush();
}

void NsecTagTimerFailFunc()
{
  Serial.println("Tag failed");
  nsec_tag_num = 0;
  nsec_tag_bool = false;
}

void NsecTagTimerSuccessFunc()
{
  Serial.println("Tag success, 2s elapsed");
  nsec_tag_num = 0;
}