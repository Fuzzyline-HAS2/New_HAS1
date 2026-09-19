#include "HAS1_revival_machine.h"

void BeginRevivalApproval(unsigned long tagDetectedMs)
{
  revival_approval_pending = true;
  revival_approval_started_ms = tagDetectedMs;
  revival_approval_last_poll_ms = millis();
  revival_approval_last_admin_poll_ms = millis();
  revival_approval_poll_due = true;
  revival_request_device_state = (String)(const char *)my["device_state"];
}

void EndRevivalApproval(const char *reason, bool preserveUser)
{
  if (revival_approval_pending)
    Serial.println(String("[Approval] wait ended: ") + reason);
  revival_approval_pending = false;
  revival_approval_poll_due = false;
  // 승인 대기 중 멈춘 일반 타이머를 재시작해 밀린 HTTP 조회가 연속 발생하지 않게 한다.
  wifi_timer.restartTimer(wifi_timer_id);
  ghost_open_pending = false;
  // HTTP 실패/시간초과만으로 서버 처리 실패가 확정되지는 않는다.
  // 늦은 open에도 원래 사용자의 is_open을 기록할 수 있도록 필요한 경우 보존한다.
  if (!preserveUser) last_open_tag_user = "";
}

// 매 루프에서 호출한다. Loop의 shift 플래그나 DataChange 발생 여부에 의존하지 않는다.
void UpdateRevivalApprovalState()
{
  if (!revival_approval_pending) return;

  String gameState = (String)(const char *)my["game_state"];
  String deviceState = (String)(const char *)my["device_state"];
  if (gameState != "activate")
  {
    EndRevivalApproval("game state changed");
  }
  else if (deviceState == "open")
  {
    // DataChange의 승인 처리에서 최초 태그의 타이밍/is_open 기록을 마칠 때까지 보존한다.
    revival_approval_pending = false;
    revival_approval_poll_due = false;
  }
  else if (deviceState != revival_request_device_state)
  {
    EndRevivalApproval("device state changed");
  }
  else if (millis() - revival_approval_started_ms >= REVIVAL_APPROVAL_TIMEOUT_MS)
  {
    if (ghost_open_pending)
      Serial.println("[GhostTiming] TIMEOUT waiting for open (" + String(REVIVAL_APPROVAL_TIMEOUT_MS) + "ms)");
    EndRevivalApproval("timeout", true);
  }
}

void PollRevivalApproval()
{
  UpdateRevivalApprovalState();
  if (!revival_approval_pending) return;
  if (!revival_approval_poll_due &&
      millis() - revival_approval_last_poll_ms < REVIVAL_APPROVAL_POLL_MS) return;

  revival_approval_poll_due = false;
  revival_approval_polled_this_loop = true;
  // 승인 대기 중에는 request=Loop/shift_machine 게이트 없이 기기 상태를 직접 조회한다.
  has2wifi.ReceiveMine();
  DataChange();
  // 느린 HTTP 요청 뒤에도 다음 요청까지 간격을 보장한다.
  revival_approval_last_poll_ms = millis();
}

void ObserveGameplayTag(bool detected)
{
  if (!gameplay_tag_latched) return;
  if (detected)
  {
    gameplay_tag_missing = false;
    gameplay_tag_miss_count = 0;
    return;
  }
  if (!gameplay_tag_missing)
  {
    gameplay_tag_missing = true;
    gameplay_tag_missing_since_ms = millis();
    gameplay_tag_miss_count = 1;
    return;
  }
  if (gameplay_tag_miss_count < 2) ++gameplay_tag_miss_count;
  if (gameplay_tag_miss_count >= 2 &&
      millis() - gameplay_tag_missing_since_ms >= RFID_REARM_ABSENT_MS)
  {
    gameplay_tag_latched = false;
    gameplay_tag_user = "";
    gameplay_tag_missing = false;
    gameplay_tag_miss_count = 0;
  }
}
