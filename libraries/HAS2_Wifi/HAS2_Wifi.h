/**
 * @file HAS2_Wifi.h
 * @author 김유빈
 * @brief
 * @version 1.0
 * @date 2022-09-28
 *
 * @copyright Copyright (c) 2022
 *
 */

#ifndef _HAS2_WIFI_
#define _HAS2_WIFI_

#include "Arduino.h"
#include <WiFi.h>
#include <HTTPClient.h>  // wifi 관련 라이브러리
#include <ArduinoJson.h> // wifi 관련 라이브러리
#include <HTTPUpdate.h>  // OTA 관련 라이브러리
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <Preferences.h>

void update_started();
void update_finished();
void update_progress(int cur, int total);
void update_error(int err);

// 전역변수를 main 파일에서 사용할 수 있게 extern 선언
extern HTTPClient http;
extern StaticJsonDocument<512> shift_machine;
extern StaticJsonDocument<2048> my;
extern StaticJsonDocument<2048> tag;
extern StaticJsonDocument<1024> skill;

typedef struct HAS2_WifiCandidate
{
    const char *ssid;
    const char *password;
    int lastRssi;
    float avgRssi;
    int seenCount;
    unsigned long lastSeenMs;
} HAS2_WifiCandidate;


/**
 * @brief HAS2 전용 Wifi 라이브러리
 *
 */
class HAS2_Wifi
{
private:
    String HOST_NAME;
    String PHP_FILE_NAME;
    String server;
    String device_name;
    String my_mac;
    Preferences wifi_preferences;
    unsigned long lastWifiScanMs;
    bool wifiCandidatesInitialized;
    bool legacy1MbpsEnabled = false;
    bool boundedResponseBodyEnabled = false;
    void (*httpDiagnosticHook)(const char *, const char *, int32_t, uint32_t, uint32_t) = nullptr;
    // Setup may have consumed the notification; validate the first fresh poll too.
    bool freshReceivePending = true;

    friend class HTTPUpdate;

    String _theme;

    bool HttpRequest(String request, String string_request, bool requireFresh = false);
    bool JsonParsingFresh(String request, String json);
    void JsonParsing(String request, String json);
    void EnsureWifiCandidatesInitialized();
    void ScanNetworks(bool force = false);
    bool ApplyLegacy1Mbps();
    bool TryConnect(const char *new_ssid, const char *new_password, unsigned long timeoutMs = 3000);
    bool TryConnectOrdered();
    bool TryConnectSaved();
    void SaveLastWifi(const char *new_ssid, const char *new_password);
    void MaintainWifi();
    void PrintConnectedWifi();

public:
    HAS2_Wifi();
    // HAS2_Wifi(String php);
    HAS2_Wifi(String host, String php = "/has2.php");

    void SetDebugPrint(Print *debugPrint);
    // Call before Setup/Connect. Applied on every connection attempt; default is off.
    void EnableLegacy1Mbps();
    // Opt-in: 1500ms body deadline, 16KiB cap, complete framing before parsing.
    // DNS/connect/HTTP headers retain HTTPClient timeouts; not a request deadline.
    void EnableBoundedResponseBody();
    // Optional synchronous-request observer; default null, no async task callbacks.
    // Callback must be nonblocking, must not make HTTP requests, and must copy any
    // strings it retains. request is an operation name, never a URL/card/body.
    // elapsedMs is unsigned time since request.begin (wrap-safe).
    // request.begin: result/detail=0; get.end: result=HTTP status/error, detail=0.
    // GET spans begin/DNS/connect/headers together, not separate phase timings.
    // body.end: result=Has2HttpBody::Result (0=Ok), -1 if skipped (transport error);
    // detail=retained decoded bytes (0 on rejected body; not bytes received).
    // close.begin/end: result=HTTP status/error; request.end: result=success 1/0.
    // These last three phases retain the same body-byte detail, even on JSON error.
    void SetHttpDiagnosticHook(void (*hook)(const char *request, const char *phase,
                                          int32_t result, uint32_t elapsedMs, uint32_t detail));

    void Setup();
    void Setup(char *new_ssid, char *new_password);
    void Setup(String theme);
    void Connect(String theme);
    void Receive(String device_name);
    void ReceiveMP3(String device_name, int value);
    void ReceiveMine();
    void Send(String device_name, String column, String value);
    void SendAsync(String device_name, String column, String value);
    bool Situation(String affected_device_name, String situation, String key_device = "");
    void SituationAsync(String affected_device_name, String situation, String key_device = "");
    void Loop();
    void Loop(void (*Func)(void));
    // Invoke callback only after a successful, validated ReceiveMine snapshot.
    void LoopFresh(void (*Func)(void));
    void FirmwareUpdate(String device_type, String ip_address = "172.30.1.43");
    String GetConnectedSSID();
    void PrintConnectedSSID();
};

#endif
