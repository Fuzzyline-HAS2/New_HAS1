#include "HAS1_altar.h"
#include <Preferences.h>

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
        // RTC 신뢰 불가 (전압강하/외부리셋) — NVS에서 마지막 위치 복원
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
        // 소프트 리셋 계열 — RTC 그대로 살아있음
        // ESP_RST_TASK_WDT / ESP_RST_PANIC → [CRASH]
        // ESP_RST_SW (ESP.restart() 등) → [RESET]
        bool is_crash = (r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
                         r == ESP_RST_INT_WDT  || r == ESP_RST_PANIC);
        g_crash_count++;

        snprintf(g_crash_msg, sizeof(g_crash_msg),
            "reason=%s uptime=%lums fn=%s loop=%lu crash#%u",
            ResetReasonStr(r), g_last_uptime_ms, g_last_fn,
            g_loop_count, (unsigned)g_crash_count);

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

// WiFi 연결 후 서버로 전송 — g_crash_pending 플래그로 한 번만 전송
void CrashReportSend(const char *device_name) {
    if (!g_crash_pending || !device_name || device_name[0] == '\0') return;
    has2wifi.Send(device_name, "crash_log", g_crash_msg);
    g_crash_pending = false;
}

// 30초마다 현재 BREADCRUMB 위치를 NVS에 flush — 전원 차단 후에도 복원 가능
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
