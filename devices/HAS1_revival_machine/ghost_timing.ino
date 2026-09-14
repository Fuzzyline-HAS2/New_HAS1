#include "HAS1_revival_machine.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// 유령 태그 -> open 확정 소요시간을 구글시트(Apps Script Web App)로 실시간 전송.
// has2wifi가 쓰는 172.30.1.43(로컬 PHP 서버)과 완전히 별개 목적지라, has2wifi 내부의
// 공유 HTTPClient를 재사용하지 않고 이 함수 전용 로컬 인스턴스를 쓴다 - has2wifi.Loop()
// 콜백(DataChange) 흐름 안에서 호출될 수 있어 재진입으로 인한 문제를 만들지 않기 위함.
static const char *GHOST_TIMING_SHEET_URL =
    "https://script.google.com/macros/s/AKfycbwANiiaBpXnNBeeU8s7ubzyptonlRv42vI2SYUsjWGfVKN02FNq6JZ2UW3aenVqidjIgQ/exec";

// 실패해도(오프라인/시트 문제 등) 조용히 무시 — 어디까지나 디버깅 보조 수단이라 게임
// 진행을 막으면 안 된다. 타임아웃을 짧게(5초) 잡아 이 호출 자체가 새로운 지연 원인이
// 되지 않도록 한다.
void SendGhostTimingToSheet(const String &tag_user, unsigned long total_ms, int poll_attempts,
                             unsigned long situation_ms, bool situation_ok,
                             int rssi_tag, int rssi_open, uint32_t free_heap, const char *note)
{
    WiFiClientSecure client;
    client.setInsecure();   // 구글 루트 인증서를 임베드하지 않고 스킵 — 디버그 로그 전송이라 허용
    client.setTimeout(5000);

    HTTPClient sheetHttp;
    sheetHttp.setConnectTimeout(5000);
    sheetHttp.setTimeout(5000);
    sheetHttp.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);  // script.google.com -> googleusercontent.com 302 리다이렉트 대응

    if (!sheetHttp.begin(client, GHOST_TIMING_SHEET_URL)) return;
    sheetHttp.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String body = "device_name=" + (String)(const char *)my["device_name"] +
                  "&tag_user=" + tag_user +
                  "&total_ms=" + String(total_ms) +
                  "&poll_attempts=" + String(poll_attempts) +
                  "&situation_ms=" + String(situation_ms) +
                  "&situation_ok=" + String(situation_ok ? 1 : 0) +
                  "&rssi_tag=" + String(rssi_tag) +
                  "&rssi_open=" + String(rssi_open) +
                  "&free_heap=" + String(free_heap) +
                  "&note=" + String(note);

    int code = sheetHttp.POST(body);
    Serial.println("[GhostTimingSheet] POST code=" + String(code));
    sheetHttp.end();
}
