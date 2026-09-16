#define FIRMWARE_VER 1
#define PARTITION_VER 1
#include "beetle.h"
#include <esp_system.h>

namespace beetle {
const int kFirmwareVersion = FIRMWARE_VER;
const int kPartitionVersion = PARTITION_VER;
HardwareSerial link(1);
std::atomic<bool> otaBusy{false};
bool scanRequested = false;
uint32_t lastTtgoFrame = 0;
uint32_t bootId = 0;
}

static esp_task_wdt_user_handle_t loopWatchdog = nullptr;
static iotglove::ResetRequestGate resetGate;
static bool resetPending = false;
static bool resetStarted = false;

static void configureWatchdog(uint32_t timeoutMs) {
  const esp_task_wdt_config_t config = {timeoutMs, 0, true};
  esp_err_t result = esp_task_wdt_reconfigure(&config);
  if (result == ESP_ERR_INVALID_STATE) result = esp_task_wdt_init(&config);
  ESP_ERROR_CHECK(result);
}

void setup() {
  Serial.begin(115200);
  pinMode(beetle_config::kResetRequest, INPUT_PULLDOWN);
  beetle::link.begin(beetle_config::kBaud, SERIAL_8N1,
                     beetle_config::kUartRx, beetle_config::kUartTx);
  beetle::bootId = esp_random();
  beetle::diagnosticsInit(static_cast<uint32_t>(esp_reset_reason()));
  Serial.printf("[beetle] firmware=%d reset_reason=%d\n", FIRMWARE_VER,
                static_cast<int>(esp_reset_reason()));
  configureWatchdog(beetle_config::kWatchdogMs);
  ESP_ERROR_CHECK(esp_task_wdt_add_user("beetle-loop", &loopWatchdog));
  beetle::otaInit();
  beetle::bleInit();
  beetle::sendHello();
  beetle::queueBootLog();
  beetle::otaReplayResult();
}

void loop() {
  const uint32_t now = millis();
  beetle::uartPoll(now);
  beetle::otaPoll(now);
  resetPending = resetGate.update(digitalRead(beetle_config::kResetRequest) == HIGH, now);
  // GPIO request remains latched throughout OTA; failure/skip services it only
  // after the worker has returned. Successful OTA consumes it by rebooting.
  if (resetPending && !beetle::otaBusy.load() && !resetStarted) {
    resetStarted = true;
    beetle::queueDiagnosticLog(iotglove::diagnostics::Event::Reset,
        iotglove::diagnostics::Code::Requested, beetle_config::kResetRequest);
    Serial.println("[beetle] remote reset requested; watchdog feed stopped");
    configureWatchdog(1000);
  }
  const bool enabled = beetle::scanRequested && !beetle::otaBusy.load() &&
      !resetStarted && now - beetle::lastTtgoFrame < beetle_config::kTtgoTimeoutMs;
  beetle::blePoll(now, enabled);

  static uint32_t lastHeartbeat = 0;
  if (now - lastHeartbeat >= beetle_config::kHeartbeatMs) {
    lastHeartbeat = now;
    beetle::sendHeartbeat();
  }
  static uint32_t lastLocation = 0;
  static uint32_t locationSequence = 0;
  if (now - lastLocation >= beetle_config::kLocationReportMs) {
    lastLocation = now;
    const auto location = beetle::currentLocation(now);
    iotglove::wire::Frame frame;
    strcpy(frame.type, "LOC");
    frame.id = ++locationSequence;
    iotglove::wire::put(frame, location.room);
    char value[16];
    snprintf(value, sizeof(value), "%d", location.rssi);
    iotglove::wire::put(frame, value);
    snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(location.ageMs));
    iotglove::wire::put(frame, value);
    iotglove::wire::put(frame, location.valid ? "1" : "0");
    beetle::sendFrame(frame);
  }
  beetle::diagnosticsPoll();
  if (!resetStarted && beetle::bleHealthy(now) && beetle::otaHealthy(now))
    ESP_ERROR_CHECK(esp_task_wdt_reset_user(loopWatchdog));
  // Yield to NimBLE, Wi-Fi, and flash worker even while waiting for a WDT reset.
  delay(2);
}
