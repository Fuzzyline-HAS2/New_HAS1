#include "iotglove_beetle.h"
#include <HAS2_Wifi.h>
#include <SecureOTA.h>
#include <Preferences.h>
#include <IoTGloveOtaClient.h>
#include "ota_record.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define HMAC_SECRET ""
#endif

namespace beetle {
namespace {
using ota_record::Record;
using ota_record::Result;
using iotglove::diagnostics::Code;
using iotglove::diagnostics::Event;
QueueHandle_t events = nullptr;
Record lastRecord;
Record activeRecord;
std::atomic<uint32_t> activeRequest{0};
uint32_t otaStarted = 0;

const char* resultName(Result value) {
  switch (value) {
    case Result::Accepted: return "accepted";
    case Result::Flashing: return "flashing";
    case Result::Updated: return "updated";
    case Result::Skipped: return "skipped";
    case Result::Disabled: return "disabled";
    default: return "failed";
  }
}

bool storeRecord(const Record& record) {
  Preferences preferences;
  if (!preferences.begin("glove-ota", false)) return false;
  const bool ok = preferences.putBytes("last", &record, sizeof(record)) == sizeof(record);
  preferences.end();
  return ok;
}

void publish(const Record& record) {
  // Only three events per request; queue is exclusively drained by loop().
  xQueueSend(events, &record, pdMS_TO_TICKS(100));
}

void worker(void*) {
  // activeRecord is immutable from xTaskCreate until the terminal event is read.
  Record record = activeRecord;
  bool skipped = false;
  // Record the attempt before any flash work. A power loss or WDT during OTA
  // therefore becomes a failed result after reboot, never an implicit success.
  if (!storeRecord(record)) {
    queueDiagnosticLog(Event::Ota, Code::NvsFail, record.request);
    record.result = Result::Failed;
    publish(record);
    vTaskDelete(nullptr);
    return;
  }
  HAS2_Wifi wifi;
  queueDiagnosticLog(Event::Ota, Code::WifiStart, record.request);
  if (wifi.TrySetupFixed("badland", "badland_shoot")) {
    queueDiagnosticLog(Event::Ota, Code::WifiOk, record.request);
    const auto onSuccess = [record]() mutable {
      // Both updaters invoke this AFTER verification/commit and BEFORE restart.
      // Completion is reported only after a new boot matches the stored target.
      record.result = Result::Flashing;
      if (!storeRecord(record)) queueDiagnosticLog(Event::Ota, Code::NvsFail, record.request);
      queueDiagnosticLog(Event::Ota, Code::Flashing, record.request);
      publish(record);
      delay(100);  // Allow loop() to transmit before SecureOTA restarts.
    };
    queueDiagnosticLog(Event::Ota, Code::Checking, record.request);
    if (record.requestedVersion) {
      skipped = iotglove::ota::checkPinned(
          "iotglove_beetle", record.requestedVersion, kFirmwareVersion,
          kPartitionVersion, HMAC_SECRET, onSuccess) == iotglove::ota::CheckResult::Skipped;
    } else {
      SecureOTA updater(
          "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/iotglove_beetle/update.bin",
          "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/iotglove_beetle/version.txt",
          "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/iotglove_beetle/update.sig",
          HMAC_SECRET, kFirmwareVersion);
      updater.setOnSkip([&]() { skipped = true; });
      updater.setOnSuccess(onSuccess);
      // Legacy latest-release command keeps its fixed tag/SecureOTA behavior.
      updater.check();
    }
  } else {
    queueDiagnosticLog(Event::Ota, Code::WifiFail, record.request);
  }
  // checkPinned only skips after signed metadata matches board, exact firmware,
  // and the USB-installed partition version. No partition OTA is enabled.
  record.result = skipped ? Result::Skipped : Result::Failed;
  if (skipped) record.targetVersion = kFirmwareVersion;
  if (!storeRecord(record)) queueDiagnosticLog(Event::Ota, Code::NvsFail, record.request);
  queueDiagnosticLog(Event::Ota, skipped ? Code::Skipped : Code::Failed, record.request);
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_OFF);
  publish(record);
  vTaskDelete(nullptr);
}
}

void otaInit() {
  events = xQueueCreate(8, sizeof(Record));
  if (!events) abort();
  Preferences preferences;
  if (!preferences.begin("glove-ota", true)) return;
  const size_t size = preferences.getBytesLength("last");
  bool migrated = false;
  if (size == sizeof(lastRecord)) {
    preferences.getBytes("last", &lastRecord, sizeof(lastRecord));
  } else if (size == sizeof(ota_record::LegacyRecord)) {
    ota_record::LegacyRecord legacy = {};
    preferences.getBytes("last", &legacy, sizeof(legacy));
    lastRecord = ota_record::migrateLegacy(legacy);
    migrated = true;
  }
  preferences.end();
  if (!ota_record::valid(lastRecord)) { lastRecord = Record{}; return; }
  const Result recovered = ota_record::recover(lastRecord, kFirmwareVersion,
                                               kPartitionVersion, bootId);
  if (ota_record::pending(lastRecord.result) || recovered != lastRecord.result || migrated) {
    lastRecord.result = recovered;
    if (recovered == Result::Updated) lastRecord.targetVersion = kFirmwareVersion;
    storeRecord(lastRecord);
  }
  if (lastRecord.result == Result::Updated || lastRecord.result == Result::Skipped ||
      lastRecord.result == Result::Failed)
    queueDiagnosticLog(Event::Ota, lastRecord.result == Result::Updated ? Code::Updated :
        (lastRecord.result == Result::Skipped ? Code::Skipped : Code::Failed), lastRecord.request);
}

void otaReplayResult() {
  if (lastRecord.request && lastRecord.result != Result::None)
    sendOtaResult(lastRecord.request, resultName(lastRecord.result));
}

void otaRequest(uint32_t requestId, uint32_t targetVersion) {
#if defined(IOTGLOVE_COMPILE_ONLY) && IOTGLOVE_COMPILE_ONLY
  if (requestId) queueDiagnosticLog(Event::Ota, Code::Disabled, requestId);
  sendOtaResult(requestId, "disabled");
  return;
#endif
  if (otaBusy.load()) {
    sendOtaResult(requestId, requestId == activeRequest.load()
        ? (ota_record::sameRequest(activeRecord, requestId, targetVersion) ? "accepted" : "failed")
        : "busy");
    return;
  }
  if (requestId == lastRecord.request) {
    if (ota_record::sameRequest(lastRecord, requestId, targetVersion)) otaReplayResult();
    else sendOtaResult(requestId, "failed");
    return;
  }
  if (!requestId || (targetVersion && !ota_record::validVersion(targetVersion))) {
    sendOtaResult(requestId, "failed");
    return;
  }
  const char* secret = HMAC_SECRET;
  if (!secret[0] || strcmp(secret, "REPLACE_WITH_DEPLOYMENT_SECRET") == 0 ||
      strcmp(secret, "__COMPILE_ONLY_DO_NOT_DEPLOY__") == 0) {
    queueDiagnosticLog(Event::Ota, Code::Disabled, requestId);
    sendOtaResult(requestId, "disabled");
    return;
  }
  activeRequest.store(requestId);
  activeRecord = Record{};
  activeRecord.request = requestId;
  activeRecord.requestedVersion = targetVersion;
  activeRecord.targetVersion = targetVersion;
  activeRecord.oldVersion = kFirmwareVersion;
  activeRecord.oldBootId = bootId;
  activeRecord.partitionVersion = kPartitionVersion;
  activeRecord.result = Result::Accepted;
  otaStarted = millis();
  otaBusy.store(true);
  lastRecord = activeRecord;
  sendOtaResult(requestId, "accepted");
  // Pause scanning before Wi-Fi/flash work starts; no main loop is blocked by HTTP.
  blePoll(millis(), false);
  if (xTaskCreate(worker, "beetle-ota", 12288, nullptr, 1, nullptr) != pdPASS) {
    queueDiagnosticLog(Event::Ota, Code::TaskFail, requestId);
    otaBusy.store(false);
    lastRecord.result = Result::Failed;
    storeRecord(lastRecord);
    sendOtaResult(requestId, "failed");
  }
}

void otaPoll(uint32_t) {
  Record record;
  while (xQueueReceive(events, &record, 0)) {
    lastRecord = record;
    if (!ota_record::pending(record.result))
      otaBusy.store(false);
    sendOtaResult(record.request, resultName(record.result));
  }
}

bool otaHealthy(uint32_t now) {
  // A hung worker is recovered by the watchdog; don't delete a task mid-flash.
  return !otaBusy.load() || now - otaStarted < beetle_config::kOtaTimeoutMs;
}
}
