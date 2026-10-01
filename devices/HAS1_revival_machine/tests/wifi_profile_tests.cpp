// Driver fake models init idempotence and full teardown losing radio settings.
// Real production methods are extracted by the runner; no Arduino/RF emulation.
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <string>

constexpr int WIFI_OFF = 0, WIFI_STA = 1, WIFI_IF_STA = 0;
constexpr int WIFI_PROTOCOL_11B = 1, WIFI_PHY_RATE_1M_L = 0;
constexpr int WL_DISCONNECTED = 0, WL_CONNECTED = 1, ESP_OK = 0;
using esp_err_t = int;
struct wifi_init_config_t {
  int static_tx_buf_num = 16, dynamic_tx_buf_num = 32, tx_buf_type = 1;
  int cache_tx_buf_num = 32, static_rx_buf_num = 10, dynamic_rx_buf_num = 32;
  int ampdu_tx_enable = 1;
};
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}

unsigned long clockMs = 0;
unsigned long millis() { return clockMs; }
void delay(unsigned long ms) { clockMs += ms; }
std::string logs;
struct Print {
  void print(const char *) {}
  void println(const char * = "") {}
} debugPrint;
Print *_has2DebugPrint = &debugPrint;
void HAS2DebugPrintf(const char *format, ...) {
  char buffer[200];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  logs += buffer;
}
const char *esp_err_to_name(int) { return "FAKE_ERROR"; }
enum class Failure { None, Off, Network, Init, Start, Protocol, Rate };
struct Driver {
  bool initialized = false, started = false, fixed = false, bOnly = false;
  bool staticBuffers = false, connectSuccess = true, expectProfile = false;
  bool connected = false, sleeping = true, customInit = false;
  Failure failure = Failure::None;
  int beginCount = 0, profileCount = 0, savedCount = 0;
  wifi_init_config_t config;
} driver;

esp_err_t esp_wifi_init(const wifi_init_config_t *config) {
  if (driver.failure == Failure::Init && config->ampdu_tx_enable == 0) return -1;
  if (!driver.initialized) {
    driver.config = *config;
    driver.initialized = true;
    driver.customInit = config->ampdu_tx_enable == 0;
  }
  return ESP_OK;
}
struct NetworkFake { bool begin() { return driver.failure != Failure::Network; } } Network;
struct WiFiFake {
  void persistent(bool value) { assert(!value); }
  void setAutoReconnect(bool value) { assert(!value); }
  bool useStaticBuffers() { return driver.staticBuffers; }
  bool mode(int mode) {
    if (mode == WIFI_OFF) {
      if (driver.failure == Failure::Off) return false;
      // Arduino may report OFF after a failed start although init succeeded.
      if (driver.initialized && !driver.started) return true;
      driver.initialized = driver.started = driver.fixed = driver.bOnly = false;
      driver.connected = driver.customInit = false;
      return true;
    }
    if (driver.failure == Failure::Start && driver.customInit) return false;
    wifi_init_config_t defaults = WIFI_INIT_CONFIG_DEFAULT();
    assert(esp_wifi_init(&defaults) == ESP_OK); // Arduino's second init is a no-op.
    driver.started = true;
    return true;
  }
  void disconnect(bool off, bool erase) {
    assert(off && erase);
    driver.connected = false;
    driver.initialized = driver.started = driver.fixed = driver.bOnly = false;
    driver.customInit = false;
  }
  int status() { return driver.connected ? WL_CONNECTED : WL_DISCONNECTED; }
  void begin(const char *, const char *) {
    if (!driver.started) mode(WIFI_STA);
    if (driver.expectProfile) {
      assert(driver.fixed && driver.bOnly && !driver.config.ampdu_tx_enable);
    } else {
      assert(!driver.fixed && !driver.bOnly && driver.config.ampdu_tx_enable);
    }
    driver.beginCount++;
    driver.connected = driver.connectSuccess;
  }
  void setSleep(bool enabled) { driver.sleeping = enabled; }
} WiFi;
esp_err_t esp_wifi_set_protocol(int iface, int protocol) {
  assert(iface == WIFI_IF_STA && protocol == WIFI_PROTOCOL_11B && driver.started);
  if (driver.failure == Failure::Protocol) return -1;
  driver.bOnly = true;
  return ESP_OK;
}
esp_err_t esp_wifi_internal_set_fix_rate(int iface, bool enabled, int rate) {
  assert(iface == WIFI_IF_STA && enabled && rate == WIFI_PHY_RATE_1M_L);
  assert(driver.started && driver.bOnly && !driver.config.ampdu_tx_enable);
  if (driver.failure == Failure::Rate) return -1;
  driver.fixed = true;
  driver.profileCount++;
  return ESP_OK;
}
class HAS2_Wifi {
  #include "wifi_profile_default.inc"
 public:
  void EnableLegacy1Mbps();
  bool ApplyLegacy1Mbps();
  bool TryConnect(const char *, const char *, unsigned long);
  void SaveLastWifi(const char *, const char *) { driver.savedCount++; }
  void PrintConnectedWifi() {}
};
#include "wifi_profile_under_test.inc"

void reset() { driver = Driver{}; logs.clear(); clockMs = 0; }
int main() {
  reset();
  HAS2_Wifi ordinary;
  assert(ordinary.TryConnect("normal", "password", 100));
  assert(driver.profileCount == 0 && !driver.sleeping && driver.savedCount == 1);

  reset();
  HAS2_Wifi revival;
  revival.EnableLegacy1Mbps();
  driver.expectProfile = true;
  assert(revival.TryConnect("saved-ap", "password", 100));
  assert(driver.config.static_tx_buf_num == 0 && driver.config.dynamic_tx_buf_num == 32);
  assert(driver.config.tx_buf_type == 1 && driver.config.cache_tx_buf_num == 4);
  assert(driver.config.static_rx_buf_num == 4 && driver.config.dynamic_rx_buf_num == 32);
  // Drop/reconnect destroys the driver, so retained per-object intent must reapply.
  WiFi.disconnect(true, true);
  assert(revival.TryConnect("saved-ap", "password", 100));
  assert(driver.profileCount == 2);
  driver.connectSuccess = false;
  assert(!revival.TryConnect("failed-ap", "password", 100));
  assert(!driver.started && !driver.fixed);
  driver.connectSuccess = true;
  assert(revival.TryConnect("fallback-ap", "password", 100));
  assert(driver.profileCount == 4 && driver.savedCount == 3);
  assert(logs.find("DSSS TX fixed at 1 Mbps") != std::string::npos);

  reset();
  driver.expectProfile = driver.staticBuffers = true;
  assert(revival.TryConnect("static-buffers", "password", 100));
  assert(driver.config.static_tx_buf_num == 16 && driver.config.static_rx_buf_num == 10);

  for (Failure failure : {Failure::Off, Failure::Network, Failure::Init,
                          Failure::Start, Failure::Protocol, Failure::Rate}) {
    reset();
    driver.expectProfile = true;
    driver.failure = failure;
    assert(!revival.TryConnect("error-ap", "password", 100));
    assert(driver.beginCount == 0 && driver.savedCount == 0);
    assert(logs.find("failed") != std::string::npos);
    assert(logs.find("DSSS TX fixed") == std::string::npos);
    driver.failure = Failure::None;
    assert(revival.TryConnect("recovered-ap", "password", 100));
    assert(driver.fixed && driver.bOnly && driver.savedCount == 1);
  }
  std::cout << "PASS: Wi-Fi default, startup, reconnect, AP fallback, buffers and six failure paths\n";
}
