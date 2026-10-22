#include "HAS1_generator.h"
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

RTC_DATA_ATTR uint32_t g_last_uptime_ms = 0;
RTC_DATA_ATTR char     g_last_fn[40]    = "";
RTC_DATA_ATTR uint32_t g_loop_count     = 0;
RTC_DATA_ATTR uint8_t  g_crash_count    = 0;

bool g_crash_pending        = false;
bool g_crash_telnet_pending = false;
char g_crash_msg[200]       = "";

static Preferences crash_prefs;
static const char *CRASH_NS = "crash";

static const char *ResetReasonStr(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

void CrashReportInit() {
    esp_reset_reason_t r = esp_reset_reason();

    if (r == ESP_RST_POWERON || r == ESP_RST_BROWNOUT || r == ESP_RST_EXT) {
        // RTC 신뢰 불가(전압강하/외부리셋 — 물리 리셋 버튼 포함) — NVS에서 마지막 위치 복원
        if (r != ESP_RST_POWERON) {
            Serial.println("[RESET] reason=" + String(ResetReasonStr(r)));
        }
        crash_prefs.begin(CRASH_NS, true);
        String saved_fn     = crash_prefs.getString("fn", "");
        uint32_t saved_up   = crash_prefs.getULong("uptime", 0);
        uint32_t saved_lp   = crash_prefs.getULong("loop", 0);
        uint8_t saved_cnt   = crash_prefs.getUChar("crashes", 0);
        String saved_reason = crash_prefs.getString("reason", "SW");
        crash_prefs.end();

        if (saved_fn.length() > 0) {
            snprintf(g_crash_msg, sizeof(g_crash_msg),
                "last_reason=%s uptime=%lums fn=%s loop=%lu total_crashes=%u",
                saved_reason.c_str(), saved_up, saved_fn.c_str(), saved_lp, saved_cnt);
            Serial.println("[LASTKNOWN] " + String(g_crash_msg));
            g_crash_pending        = true;
            g_crash_telnet_pending = true;
        }

        // 읽은 후 NVS 초기화 (다음 정상 부팅 때 오염 방지)
        crash_prefs.begin(CRASH_NS, false);
        crash_prefs.clear();
        crash_prefs.end();
    }
    else if (r != ESP_RST_DEEPSLEEP) {
        // 소프트 리셋 계열 — RTC 그대로 살아있는 게 정상이지만, 실측(2026-09-14)에서
        // TASK_WDT 재부팅 후에도 g_last_fn/g_last_uptime_ms/g_crash_count가 초기값으로
        // 읽히는 경우가 있었다(정확한 원인 불명 - 워치독 패닉이 RTC 도메인까지 건드렸거나
        // 재초기화 타이밍 이슈로 추정). RTC가 비어있으면 30초마다 flush해둔 NVS 백업으로
        // 대체해 "마지막 위치 정보 자체가 통째로 날아가는" 상황을 막는다.
        // ESP_RST_TASK_WDT / ESP_RST_PANIC → [CRASH] (배선을 빠르게 뺐다 꽂았다 반복하다
        // loop()가 멈추는 문제 추적용 — 워치독이 30초 안에 못 돌면 여기로 잡힌다)
        // ESP_RST_SW (ESP.restart() 등) → [RESET]
        bool is_crash = (r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
                         r == ESP_RST_INT_WDT  || r == ESP_RST_PANIC);
        g_crash_count++;

        bool rtc_looks_valid = (g_last_fn[0] != '\0');
        String fn_to_use      = rtc_looks_valid ? String(g_last_fn) : "";
        uint32_t uptime_to_use = g_last_uptime_ms;
        uint32_t loop_to_use   = g_loop_count;
        bool used_nvs_fallback = false;

        if (!rtc_looks_valid) {
            crash_prefs.begin(CRASH_NS, true);
            fn_to_use       = crash_prefs.getString("fn", "");
            uptime_to_use   = crash_prefs.getULong("uptime", 0);
            loop_to_use     = crash_prefs.getULong("loop", 0);
            crash_prefs.end();
            used_nvs_fallback = fn_to_use.length() > 0;
        }

        snprintf(g_crash_msg, sizeof(g_crash_msg),
            "reason=%s uptime=%lums fn=%s loop=%lu crash#%u%s",
            ResetReasonStr(r), uptime_to_use, fn_to_use.c_str(),
            loop_to_use, (unsigned)g_crash_count,
            used_nvs_fallback ? " nvs_fallback_rtc_lost" : (rtc_looks_valid ? "" : " no_data"));

        Serial.println((is_crash ? "[CRASH] " : "[RESET] ") + String(g_crash_msg));
        g_crash_pending        = true;
        g_crash_telnet_pending = true;

        // NVS에 즉시 저장 — 30초 throttle 기다리지 않음
        // 전원이 차단돼도 다음 부팅 때 [LASTKNOWN]으로 복원됨
        crash_prefs.begin(CRASH_NS, false);
        crash_prefs.putUChar("crashes",  g_crash_count);
        crash_prefs.putULong("uptime",   g_last_uptime_ms);
        crash_prefs.putString("fn",      String(g_last_fn));
        crash_prefs.putULong("loop",     g_loop_count);
        crash_prefs.putString("reason",  String(ResetReasonStr(r)));
        crash_prefs.end();
    }

    g_loop_count = 0;
}

// HAS2_Wifi::Send()는 value를 URL 인코딩 없이 GET 쿼리에 그대로 붙인다
// (libraries/HAS2_Wifi/HAS2_Wifi.cpp — first_store 실물도 동일). 그래서 crash_log에
// 공백/'#'/'='가 들어가면 HTTP 요청라인이 깨지거나 쿼리 파싱이 오염된다.
// unreserved(A-Z a-z 0-9 - _ . ~) 외의 문자를 '_'로 치환해 형식은 읽을 수 있게 유지한다.
static void UrlSafeInPlace(char *s) {
    for (; *s; ++s) {
        char c = *s;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (!ok) *s = '_';
    }
}

// External Google Sheet uploads are intentionally disabled. An optional
// diagnostic destination must not block startup or the game loop; bounded local
// RTC/Telnet evidence remains available even when the server cannot be reached.

// WiFi 연결 후 서버로 전송 — g_crash_pending 플래그로 한 번만 전송
void CrashReportSend(const char *device_name) {
    if (!g_crash_pending || !device_name || device_name[0] == '\0') return;
    UrlSafeInPlace(g_crash_msg);   // 전송 직전 1회 — Serial/Telnet 출력은 원문 그대로 이미 나갔다
    // A full queue leaves this report pending. Accepted reports are owned by
    // the bounded network worker; local RTC/Telnet evidence is not cleared.
    if (GeneratorNetworkSend(device_name, "crash_log", g_crash_msg)) {
        g_crash_pending = false;
    }
}

// 30초마다 현재 BREADCRUMB 위치를 NVS에 flush — 전원 차단/물리 리셋 후에도 복원 가능
void CrashNvsFlush() {
    static unsigned long last_flush = 0;
    unsigned long now = millis();
    if (now - last_flush < 30000) return;
    last_flush = now;

    crash_prefs.begin(CRASH_NS, false);
    crash_prefs.putULong("uptime", g_last_uptime_ms);
    crash_prefs.putString("fn", String(g_last_fn));
    crash_prefs.putULong("loop", g_loop_count);
    crash_prefs.putUChar("crashes", g_crash_count);
    crash_prefs.end();
}
