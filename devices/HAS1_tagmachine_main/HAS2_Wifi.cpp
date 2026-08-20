/**
 * @file HAS2_Wifi.cpp
 * @author 김유빈
 * @brief
 * @version 1.0
 * @date 2022-09-28
 *
 * @copyright Copyright (c) 2022
 *
 */

#include "HAS2_Wifi.h"

static String _activeHost = "http://172.30.1.43";
static Print *_has2DebugPrint = &Serial;
static const int WIFI_COUNT = 3;
static const int WIFI_MIN_RSSI = -70;
static const unsigned long WIFI_SCAN_INTERVAL_MS = 60000;
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 3000;
static const char *WIFI_PREF_NAMESPACE = "has2wifi";
static const char *WIFI_PREF_SSID = "last_ssid";
static const char *WIFI_PREF_PASSWORD = "last_pw";

static HAS2_WifiCandidate badland_wifi_candidates[WIFI_COUNT] = {
    {"badland_ruins", "Code3824@", -127, -127.0f, 0, 0},
    {"badland_auto",  "Code3824@", -127, -127.0f, 0, 0},
    {"badland_shoot", "Code3824@", -127, -127.0f, 0, 0},
};

static HAS2_WifiCandidate city_wifi_candidates[WIFI_COUNT] = {
    {"bar",    "Code3824@", -127, -127.0f, 0, 0},
    {"office", "Code3824@", -127, -127.0f, 0, 0},
    {"gun",    "Code3824@", -127, -127.0f, 0, 0},
};

static void HAS2DebugPrintf(const char *format, ...)
{
  if (_has2DebugPrint == nullptr)
  {
    return;
  }

  char buffer[180];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  _has2DebugPrint->print(buffer);
}

/**
 * @brief 디버그 출력 대상을 설정 (기본값: Serial)
 *
 * @param debugPrint 디버그 메시지를 출력할 Print 객체 포인터. nullptr이면 출력 비활성화
 */
void HAS2_Wifi::SetDebugPrint(Print *debugPrint)
{
  _has2DebugPrint = debugPrint;
}

/**
 * @brief HAS2_Wifi 기본생성자
 *
 */
HAS2_Wifi::HAS2_Wifi()
    : HOST_NAME("http://172.30.1.43"),
      PHP_FILE_NAME("/has2.php"),
      server(HOST_NAME + PHP_FILE_NAME),
      _theme("badland"),
      lastWifiScanMs(0),
      wifiCandidatesInitialized(false)
{
}

// /**
//  * @brief HAS2_Wifi PHP 변경 생성자
//  *
//  * @param php 원하는 PHP 파일 입력["/test.php"]형식
//  */
// HAS2_Wifi::HAS2_Wifi(String php)
//     : HOST_NAME("http://172.30.1.43"),
//       PHP_FILE_NAME(php),
//       server(HOST_NAME + PHP_FILE_NAME)
// {
// }

/**
 * @brief HAS2_Wifi PHP 변경 생성자
 *
 * @param host 로컬호스트 주소 입력["http://172.30.1.59"]형식
 * @param php 원하는 PHP 파일 입력["/test.php"]형식
 */
HAS2_Wifi::HAS2_Wifi(String host, String php)
    : HOST_NAME(host),
      PHP_FILE_NAME(php),
      server(HOST_NAME + PHP_FILE_NAME),
      _theme("badland"),
      lastWifiScanMs(0),
      wifiCandidatesInitialized(false)
{
    _activeHost = host;
}

void HAS2_Wifi::EnsureWifiCandidatesInitialized()
{
  if (wifiCandidatesInitialized)
  {
    return;
  }

  for (int i = 0; i < WIFI_COUNT; i++)
  {
    badland_wifi_candidates[i].lastRssi = -127;
    badland_wifi_candidates[i].avgRssi = -127.0f;
    badland_wifi_candidates[i].seenCount = 0;
    badland_wifi_candidates[i].lastSeenMs = 0;

    city_wifi_candidates[i].lastRssi = -127;
    city_wifi_candidates[i].avgRssi = -127.0f;
    city_wifi_candidates[i].seenCount = 0;
    city_wifi_candidates[i].lastSeenMs = 0;
  }
  wifiCandidatesInitialized = true;
}

void HAS2_Wifi::ScanNetworks(bool force)
{
  EnsureWifiCandidatesInitialized();
  WiFi.mode(WIFI_STA);

  unsigned long now = millis();
  if (!force && lastWifiScanMs != 0 && now - lastWifiScanMs < WIFI_SCAN_INTERVAL_MS)
  {
    return;
  }
  lastWifiScanMs = now;

  HAS2_WifiCandidate *candidates = (_theme == "city") ? city_wifi_candidates : badland_wifi_candidates;

  int network_count = WiFi.scanNetworks();
  if (network_count < 0)
  {
    _has2DebugPrint->println("WiFi scan failed");
    return;
  }

  for (int i = 0; i < network_count; i++)
  {
    String found_ssid = WiFi.SSID(i);
    int found_rssi = WiFi.RSSI(i);

    for (int candidate_index = 0; candidate_index < WIFI_COUNT; candidate_index++)
    {
      HAS2_WifiCandidate &candidate = candidates[candidate_index];
      if (found_ssid == candidate.ssid)
      {
        candidate.lastRssi = found_rssi;
        candidate.avgRssi = candidate.seenCount == 0 ? found_rssi : (candidate.avgRssi * 0.7f + found_rssi * 0.3f);
        candidate.seenCount++;
        candidate.lastSeenMs = now;
      }
    }
  }

  // [DEBUG] 정렬 전 원본 스캔 결과 - 3개 후보의 실제 RSSI 확인용
  _has2DebugPrint->println("[DEBUG] Scan results (pre-sort):");
  for (int i = 0; i < WIFI_COUNT; i++)
  {
    _has2DebugPrint->print("  ");
    _has2DebugPrint->print(candidates[i].ssid);
    _has2DebugPrint->print(" seenCount=");
    _has2DebugPrint->print(candidates[i].seenCount);
    _has2DebugPrint->print(" lastRssi=");
    _has2DebugPrint->print(candidates[i].lastRssi);
    _has2DebugPrint->print(" avgRssi=");
    _has2DebugPrint->println(candidates[i].avgRssi);
  }

  for (int i = 0; i < WIFI_COUNT - 1; i++)
  {
    for (int j = i + 1; j < WIFI_COUNT; j++)
    {
      bool should_swap = false;
      if (candidates[j].seenCount > 0 && candidates[i].seenCount == 0)
      {
        should_swap = true;
      }
      else if (candidates[j].seenCount > 0 && candidates[i].seenCount > 0 &&
               candidates[j].avgRssi > candidates[i].avgRssi)
      {
        should_swap = true;
      }

      if (should_swap)
      {
        HAS2_WifiCandidate temp = candidates[i];
        candidates[i] = candidates[j];
        candidates[j] = temp;
      }
    }
  }

  WiFi.scanDelete();
}

bool HAS2_Wifi::TryConnect(const char *new_ssid, const char *new_password, unsigned long timeoutMs)
{
  _has2DebugPrint->print("Try WiFi: ");
  _has2DebugPrint->println(new_ssid);

  // ESP-IDF가 NVS에 저장된 이전 접속정보로 백그라운드 자동 재연결을 시도하는 동안
  // WiFi.begin()이 호출되면 "sta is connecting, cannot set config" 에러와 함께
  // 연결이 계속 실패하는 문제가 있어 persistent/autoReconnect를 끔
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);

  // disconnect가 비동기라 곧바로 begin()을 호출하면 "sta is connecting" 상태와
  // 충돌할 수 있으므로 실제로 끊어질 때까지 짧게 대기
  unsigned long disconnect_started_ms = millis();
  while (WiFi.status() != WL_DISCONNECTED && millis() - disconnect_started_ms < 1000)
  {
    delay(20);
  }

  WiFi.begin(new_ssid, new_password);

  unsigned long started_ms = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started_ms < timeoutMs)
  {
    delay(100);
    _has2DebugPrint->print(".");
  }
  _has2DebugPrint->println();

  if (WiFi.status() == WL_CONNECTED)
  {
    SaveLastWifi(new_ssid, new_password);
    PrintConnectedWifi();
    return true;
  }

  // 실패한 연결 시도를 정리하지 않으면 STA가 connecting 상태로 남아
  // 이어지는 ScanNetworks()의 WiFi.scanNetworks()가 음수를 반환한다("WiFi scan failed").
  // begin() 앞의 teardown과 동일한 처리를 실패 경로에도 적용.
  WiFi.disconnect(true, true);
  unsigned long teardown_started_ms = millis();
  while (WiFi.status() != WL_DISCONNECTED && millis() - teardown_started_ms < 1000)
  {
    delay(20);
  }

  _has2DebugPrint->println("WiFi connect failed");
  return false;
}

bool HAS2_Wifi::TryConnectOrdered()
{
  ScanNetworks(true);

  HAS2_WifiCandidate *candidates = (_theme == "city") ? city_wifi_candidates : badland_wifi_candidates;

  for (int i = 0; i < WIFI_COUNT; i++)
  {
    HAS2_WifiCandidate &candidate = candidates[i];
    if (candidate.seenCount == 0 || candidate.lastSeenMs < lastWifiScanMs)
    {
      _has2DebugPrint->print("Skip unseen WiFi: ");
      _has2DebugPrint->println(candidate.ssid);
      continue;
    }

    if (candidate.avgRssi < WIFI_MIN_RSSI)
    {
      _has2DebugPrint->print("Skip weak WiFi: ");
      _has2DebugPrint->print(candidate.ssid);
      _has2DebugPrint->print(" RSSI=");
      _has2DebugPrint->println(candidate.avgRssi);
      continue;
    }

    if (TryConnect(candidate.ssid, candidate.password, WIFI_CONNECT_TIMEOUT_MS))
    {
      return true;
    }
  }

  return false;
}

bool HAS2_Wifi::TryConnectSaved()
{
  wifi_preferences.begin(WIFI_PREF_NAMESPACE, true);
  String saved_ssid = wifi_preferences.getString(WIFI_PREF_SSID, "");
  String saved_password = wifi_preferences.getString(WIFI_PREF_PASSWORD, "");
  wifi_preferences.end();

  if (saved_ssid.length() == 0 || saved_password.length() == 0)
  {
    return false;
  }

  _has2DebugPrint->print("Try saved WiFi: ");
  _has2DebugPrint->println(saved_ssid);
  return TryConnect(saved_ssid.c_str(), saved_password.c_str(), WIFI_CONNECT_TIMEOUT_MS);
}

void HAS2_Wifi::SaveLastWifi(const char *new_ssid, const char *new_password)
{
  wifi_preferences.begin(WIFI_PREF_NAMESPACE, false);
  wifi_preferences.putString(WIFI_PREF_SSID, new_ssid);
  wifi_preferences.putString(WIFI_PREF_PASSWORD, new_password);
  wifi_preferences.end();
}

void HAS2_Wifi::MaintainWifi()
{
  // 연결 중일 때는 블로킹 스캔 건너뜀 (WiFi.scanNetworks()는 2~4초 loop 정지)
  if (WiFi.status() == WL_CONNECTED)
  {
    return;
  }

  // 끊겼을 때만 스캔 후 재연결
  ScanNetworks(true);
  _has2DebugPrint->println("WiFi disconnected. Reconnecting...");
  if (!TryConnectOrdered())
  {
    _has2DebugPrint->println("Restart ESP");
    ESP.restart();
  }
}

/**
 * @brief 현재 연결된 WiFi의 SSID 이름을 반환
 *
 * @return String 연결되어 있지 않으면 빈 문자열
 */
String HAS2_Wifi::GetConnectedSSID()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    return "";
  }
  return WiFi.SSID();
}

/**
 * @brief 현재 연결된 WiFi의 SSID 이름을 디버그 출력으로 표시
 *
 */
void HAS2_Wifi::PrintConnectedSSID()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    _has2DebugPrint->println("WiFi not connected");
    return;
  }
  _has2DebugPrint->print("Connected SSID: ");
  _has2DebugPrint->println(WiFi.SSID());
}

void HAS2_Wifi::PrintConnectedWifi()
{
  _has2DebugPrint->println("WiFi connected");
  _has2DebugPrint->print("Connected SSID: ");
  _has2DebugPrint->println(WiFi.SSID());
  _has2DebugPrint->print("Connected RSSI: ");
  _has2DebugPrint->println(WiFi.RSSI());
  _has2DebugPrint->print("Connected to WiFi network with IP Address: ");
  _has2DebugPrint->println(WiFi.localIP());
}

/**
 * @brief Wifi 연결 및 초기설정
 *
 */
void HAS2_Wifi::Setup()
{
  if (!TryConnectSaved() && !TryConnectOrdered())
  {
    _has2DebugPrint->println("Restart ESP");
    ESP.restart();
  }
  delay(1000);

  my_mac = WiFi.macAddress();
  _has2DebugPrint->print("MY MAC=");
  _has2DebugPrint->println(my_mac);

  ReceiveMine();

  _has2DebugPrint->print("DeviceName : ");
  _has2DebugPrint->println((const char *)my["device_name"]);
}

void HAS2_Wifi::Connect(String theme)
{
  _theme = theme;
  HOST_NAME = (_theme == "city") ? "http://172.30.1.44" : "http://172.30.1.43";
  _activeHost = HOST_NAME;
  server = HOST_NAME + PHP_FILE_NAME;
  if (WiFi.status() == WL_CONNECTED)
  {
    return;
  }

  if (!TryConnectOrdered())
  {
    _has2DebugPrint->println("Restart ESP");
    ESP.restart();
  }
}

/**
 * @brief Wifi 연결 및 초기설정
 *
 */
void HAS2_Wifi::Setup(char *new_ssid, char *new_password)
{
  _has2DebugPrint->print("SSID : ");
  _has2DebugPrint->println((const char *)new_ssid);
  my_mac = WiFi.macAddress();
  _has2DebugPrint->print("MY MAC=");
  _has2DebugPrint->println(my_mac);

  if (!TryConnect((const char *)new_ssid, (const char *)new_password, WIFI_CONNECT_TIMEOUT_MS))
  {
    _has2DebugPrint->println("Restart ESP");
    ESP.restart();
  }
  delay(1000);

  my_mac = WiFi.macAddress();
  _has2DebugPrint->print("MY MAC=");
  _has2DebugPrint->println(my_mac);

  ReceiveMine();

  _has2DebugPrint->print("DeviceName : ");
  _has2DebugPrint->println((const char *)my["device_name"]);
}

void HAS2_Wifi::Setup(String theme)
{
  _theme = theme;
  HOST_NAME = (_theme == "city") ? "http://172.30.1.44" : "http://172.30.1.43";
  _activeHost = HOST_NAME;
  server = HOST_NAME + PHP_FILE_NAME;
  _has2DebugPrint->print("WiFi theme: ");
  _has2DebugPrint->println(theme);

  if (!TryConnectSaved() && !TryConnectOrdered())
  {
    _has2DebugPrint->println("Restart ESP");
    ESP.restart();
  }
  delay(1000);

  my_mac = WiFi.macAddress();
  _has2DebugPrint->print("MY MAC=");
  _has2DebugPrint->println(my_mac);

  ReceiveMine();

  _has2DebugPrint->print("DeviceName : ");
  _has2DebugPrint->println((const char *)my["device_name"]);
}
/**
 * @brief 다른 장치의 데이터를 읽음
 *
 * @param device_name 다른 장치의 이름
 */
void HAS2_Wifi::Receive(String device_name)
{
  String string_request = server + "?request=" + "Receive" + "&table=" + "device" + "&key=" + device_name;
  HttpRequest("Receive", string_request);
}

/**
 * @brief [메인 프로그램 전용] mp3재생 장치의 데이터를 읽음
 *
 * @param device_name 장치의 이름
 * @param value mp3 재생 순서
 */
void HAS2_Wifi::ReceiveMP3(String device_name, int value)
{
  String string_request = server + "?request=" + "ReceiveMP3" + "&table=" + "device" + "&key=" + device_name + "&value=" + value;
  HttpRequest("Receive", string_request);
}

/**
 * @brief 원하는 장치의 데이터를 수정
 *
 * @param device_name 데이터 변경을 당하는 장치의 이름
 * @param column 변경할 데이터의 컬럼
 * @param value 변경할 데이터의 값
 */
void HAS2_Wifi::Send(String device_name, String column, String value)
{
  String string_request = server + "?request=" + "Send" + "&table=" + "device" + "&key=" + device_name + "&column=" + column + "&value=" + value;
  HttpRequest("Send", string_request);
}

/**
 * @brief [private] 비동기 Send용 태스크 인자
 */
struct HAS2_AsyncSendParams
{
  String url;
};

/**
 * @brief [private] 응답을 쓰지 않는 Send를 별도 태스크(Core 0)에서 처리.
 * 공유 전역 HTTPClient(http)를 쓰지 않고 로컬 인스턴스를 써서 메인 루프의
 * 다른 통신(Receive/ReceiveMine/Loop 등)과 경쟁하지 않도록 함.
 */
static void HAS2_AsyncSendTask(void *pvParameters)
{
  HAS2_AsyncSendParams *params = (HAS2_AsyncSendParams *)pvParameters;

  HTTPClient localHttp;
  localHttp.begin(params->url);
  localHttp.GET();
  localHttp.end();

  delete params;
  vTaskDelete(nullptr);
}

/**
 * @brief 응답값을 쓰지 않는 리포트성 Send를 메인 루프를 막지 않고 즉시 전송
 * (게이지 애니메이션이 진행되는 동안에도 지연/멈춤 없이 서버로 도착)
 *
 * @param device_name 데이터 변경을 당하는 장치의 이름
 * @param column 변경할 데이터의 컬럼
 * @param value 변경할 데이터의 값
 */
void HAS2_Wifi::SendAsync(String device_name, String column, String value)
{
  String url = server + "?request=" + "Send" + "&table=" + "device" + "&key=" + device_name + "&column=" + column + "&value=" + value;

  HAS2_AsyncSendParams *params = new HAS2_AsyncSendParams();
  params->url = url;

  BaseType_t created = xTaskCreatePinnedToCore(
      HAS2_AsyncSendTask, "has2_async_send", 6144, params, 1, nullptr, 0);

  if (created != pdPASS)
  {
    _has2DebugPrint->println("HAS2_AsyncSendTask create failed, fallback to blocking Send");
    delete params;
    HttpRequest("Send", url); // 태스크 생성 실패 시에만 블로킹으로 대체
  }
}

/**
 * @brief
 *
 * @param affected_device_name 영향을 받는 장치
 * @param situation  상황
 * @param key_device 키로 사용할 장치. 생략하면 자신의 장치 이름 사용
 */
bool HAS2_Wifi::Situation(String affected_device_name, String situation, String key_device)
{
  String key = key_device.length() ? key_device : (String)(const char *)my["device_name"];
  String string_request = server + "?request=" + "Situation" + "&table=" + situation + "&key=" + key + "&value=" + affected_device_name;
  return HttpRequest("Send", string_request);
}

/**
 * @brief 자신의 데이터를 읽음
 *
 */
void HAS2_Wifi::ReceiveMine()
{
  String string_request = server + "?request=" + "ReceiveMine" + "&table=" + "device" + "&mac=" + my_mac;
  HttpRequest("ReceiveMine", string_request);
}

/**
 * @brief 반복적으로 ShfitMachin의 데이터를 읽음
 *
 */
void HAS2_Wifi::Loop()
{
  MaintainWifi();

  String string_request = server + "?request=" + "Loop" + "&table=" + "device" + "&mac=" + my_mac;
  HttpRequest("Loop", string_request);
  if ((int)shift_machine["shift_machine"] >= 1)
  {
    ReceiveMine();
  }
  if ((int)shift_machine["watchdog"] >= 1)
  {
    Send((String)(const char *)my["device_name"], "watchdog", "0");
    ESP.restart();
  }
}

/**
 * @brief 반복적으로 ShfitMachin의 데이터를 읽음
 *
 * @param Func shift_machine가 1 이상이 되면 실행되는 함수포인터
 */
void HAS2_Wifi::Loop(void (*Func)(void))
{
  MaintainWifi();

  String string_request = server + "?request=" + "Loop" + "&table=" + "device" + "&mac=" + my_mac;
  HttpRequest("Loop", string_request);
  if ((int)shift_machine["shift_machine"] >= 1)
  {
    ReceiveMine();
    Func();
  }
  if ((int)shift_machine["watchdog"] >= 1)
  {
    Send((String)(const char *)my["device_name"], "watchdog", "0");
    ESP.restart();
  }
  if ((String)(const char *)my["device_state"] == "update")
  {
    FirmwareUpdate((String)(const char *)my["device_type"], HOST_NAME.substring(7));
  }
}

/**
 * @brief [private] Http 통신
 *
 * @param request 원하는 명령
 * @param string_request Http에게 보내는 형식 문자열
 */
bool HAS2_Wifi::HttpRequest(String request, String string_request)
{
  //   int httpRequestCnt = 0;
  // ReRequsetHttp:

  http.begin(string_request); // 요청을 PHP로 전송
  http.setReuse(true);        // 같은 서버로 가는 연속 요청은 TCP 연결 재사용(핸드셰이크 생략)으로 왕복 단축

  int httpcode = http.GET();
  bool ok = false;

  if (httpcode > 0)
  {
    if (httpcode == HTTP_CODE_OK)
    {
      String payload = http.getString();
      if (request != "Loop")
      {
        _has2DebugPrint->println(payload);
      }
      if (request != "Send")
      {
        JsonParsing(request, payload);
      }
      ok = true;
    }
    else
    {
      _has2DebugPrint->printf("HTTP GET... code: %d\n", httpcode);
    }
  }
  else
  {
    _has2DebugPrint->printf("HTTP GET... failed, error: %s\n", http.errorToString(httpcode).c_str());
    // if(httpRequestCnt < 2){
    //   _has2DebugPrint->printf("HTTP GET... failed, error: %s\n", http.errorToString(httpcode).c_str());
    //   _has2DebugPrint->printf("Rerequest count: %d\n",httpRequestCnt);
    //   httpRequestCnt++;
    //   goto ReRequsetHttp;
    // }
    // else{
    //   _has2DebugPrint->printf("HTTP GET... failed, error: %s\n", http.errorToString(httpcode).c_str());
    //   _has2DebugPrint->printf("Maximum request exceed!");
    // }
  }
  http.end();
  return ok;
}

/**
 * @brief [private] Json 디코딩하여 원하는 데이터를 읽어옴
 *
 * @param request 원하는 명령
 * @param json String 형식의 json 파일
 */
void HAS2_Wifi::JsonParsing(String request, String json)
{
  if (request == "Loop")
  {
    auto error = deserializeJson(shift_machine, json);
    if (error)
    {
      _has2DebugPrint->print(F("deserializeJson() failed with code "));
      _has2DebugPrint->println(error.c_str());
    }
  }
  else if (request == "ReceiveMine")
  {
    auto error = deserializeJson(my, json);
    if (error)
    {
      _has2DebugPrint->print(F("deserializeJson() failed with code "));
      _has2DebugPrint->println(error.c_str());
    }
  }
  else if (request == "Receive")
  {
    auto error = deserializeJson(tag, json);
    if (error)
    {
      _has2DebugPrint->print(F("deserializeJson() failed with code "));
      _has2DebugPrint->println(error.c_str());
    }
  }
}

void HAS2_Wifi::FirmwareUpdate(String device_type, String ip_address)
{
  WiFiClient client;

  httpUpdate.onStart(update_started);
  httpUpdate.onEnd(update_finished);
  httpUpdate.onProgress(update_progress);
  httpUpdate.onError(update_error);

  String bin_file_name = "/" + device_type + ".bin";
  _has2DebugPrint->println(bin_file_name);
  t_httpUpdate_return ret = httpUpdate.update(client, ip_address, 80, bin_file_name);

  switch (ret)
  {
  case HTTP_UPDATE_FAILED:
    _has2DebugPrint->printf("HTTP_UPDATE_FAILED Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
    break;

  case HTTP_UPDATE_NO_UPDATES:
    _has2DebugPrint->println("HTTP_UPDATE_NO_UPDATES");
    break;

  case HTTP_UPDATE_OK:
    _has2DebugPrint->println("HTTP_UPDATE_OK");
    break;
  }
}

void update_started()
{
  _has2DebugPrint->println("CALLBACK:  HTTP update process started");
}

void update_finished()
{
  HAS2_Wifi has2_wifi(_activeHost);
  has2_wifi.Send((String)(const char *)my["device_name"], "device_state", "setting");
  _has2DebugPrint->println("CALLBACK:  HTTP update process finished");
}

void update_progress(int cur, int total)
{
  _has2DebugPrint->printf("CALLBACK:  HTTP update process at %d of %d bytes...\n", cur, total);
}

void update_error(int err)
{
  _has2DebugPrint->printf("CALLBACK:  HTTP update fatal error code %d\n", err);
}

// 전역변수 선언
HTTPClient http;
StaticJsonDocument<512> shift_machine;
StaticJsonDocument<2048> my;
StaticJsonDocument<2048> tag;
StaticJsonDocument<1024> skill;
