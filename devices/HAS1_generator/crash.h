#ifndef CRASH_H
#define CRASH_H

#include <Arduino.h>
#include <esp_system.h>

extern RTC_DATA_ATTR uint32_t g_last_uptime_ms;
extern RTC_DATA_ATTR char     g_last_fn[40];
extern RTC_DATA_ATTR uint32_t g_loop_count;
extern RTC_DATA_ATTR uint8_t  g_crash_count;
extern bool g_crash_pending;
extern bool g_crash_telnet_pending;
extern char g_crash_msg[200];

// 주요 함수 진입 시 호출 — 데드락 발생 위치 추적용
#define BREADCRUMB(name) do { \
    g_last_uptime_ms = millis(); \
    strncpy(g_last_fn, (name), sizeof(g_last_fn) - 1); \
    g_last_fn[sizeof(g_last_fn) - 1] = '\0'; \
} while(0)

void CrashReportInit();
void CrashReportSend(const char *device_name);
void CrashNvsFlush();

#endif
