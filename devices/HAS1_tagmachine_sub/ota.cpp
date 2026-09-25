#include "HAS1_tagmachine_sub.h"

#include <HAS2_Wifi.h>
#include <IoTGloveOtaClient.h>
#include <IoTGloveOta.h>
#include <Preferences.h>
#include <WiFi.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "ota_record.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define HMAC_SECRET ""
#endif

using tagmachine::ota_wire::Outcome;

namespace {
using tagmachine::ota_record::Record;
using tagmachine::ota_record::Result;

constexpr const char *kOtaNamespace = "tagbeetle-ota";
constexpr const char *kOtaRecordKey = "last";

QueueHandle_t otaEvents = nullptr;
Record lastRecord;
Record activeRecord;
std::atomic<bool> otaBusy{false};
std::atomic<uint32_t> activeRequest{0};
uint32_t otaStartedMs = 0;

Outcome WireOutcome(Result result) {
    switch (result) {
        case Result::Accepted: return Outcome::Accepted;
        case Result::Flashing: return Outcome::Flashing;
        case Result::Updated: return Outcome::Updated;
        case Result::Skipped: return Outcome::Skipped;
        case Result::Disabled: return Outcome::Disabled;
        default: return Outcome::Failed;
    }
}

bool StoreRecord(const Record &record) {
    Preferences preferences;
    if (!preferences.begin(kOtaNamespace, false)) return false;
    const bool stored =
        preferences.putBytes(kOtaRecordKey, &record, sizeof(record)) ==
        sizeof(record);
    preferences.end();
    return stored;
}

void PublishRecord(const Record &record) {
    // A request publishes at most flashing + one terminal event. The UART is
    // owned by loop(); the worker never writes it directly.
    if (otaEvents) xQueueSend(otaEvents, &record, pdMS_TO_TICKS(100));
}

bool OtaSecretConfigured() {
    const char *secret = HMAC_SECRET;
    return secret && secret[0] && !strstr(secret, "COMPILE_ONLY") &&
           !strstr(secret, "PLACEHOLDER") &&
           strncmp(secret, "REPLACE_WITH_", 13) != 0 &&
           strcmp(secret, "CHANGE_THIS_TO_YOUR_SECRET") != 0;
}

void OtaWorker(void *) {
    // Immutable from task creation until loop() consumes a terminal record.
    Record record = activeRecord;
    bool skipped = false;

    // Persist intent before any network/flash work. A reset while Accepted or
    // Flashing is reconciled from the running version and boot ID at startup.
    if (!StoreRecord(record)) {
        record.result = Result::Failed;
        PublishRecord(record);
        vTaskDelete(nullptr);
        return;
    }

    HAS2_Wifi wifi;
    if (wifi.TrySetup("badland")) {
        uint32_t targetVersion = record.requestedVersion;
        const bool targetResolved = targetVersion ||
            iotglove::ota::fetchLatestTarget(
                "HAS1_tagmachine_sub", tagmachinePartitionVersion,
                HMAC_SECRET, targetVersion);
        if (targetResolved &&
            iotglove::ota::monotonicTarget(tagmachineFirmwareVersion,
                                           targetVersion)) {
            // Bind power-loss recovery to the exact authenticated channel
            // target before any image bytes can be written.
            record.targetVersion = targetVersion;
            if (StoreRecord(record)) {
                const auto onSuccess = [record]() mutable {
                    // Invoked only after signed metadata, image HMAC, and OTA
                    // partition commit all succeed, immediately before reboot.
                    record.result = Result::Flashing;
                    StoreRecord(record);
                    PublishRecord(record);
                    delay(100);
                };
                skipped = iotglove::ota::checkPinned(
                    "HAS1_tagmachine_sub", targetVersion,
                    tagmachineFirmwareVersion, tagmachinePartitionVersion,
                    HMAC_SECRET, onSuccess) ==
                    iotglove::ota::CheckResult::Skipped;
            }
        }
    }

    // Successful flashing never returns. A normal return is either an exact,
    // signed-manifest same-version skip or a connection/verification failure.
    record.result = skipped ? Result::Skipped : Result::Failed;
    if (skipped) record.targetVersion = tagmachineFirmwareVersion;
    StoreRecord(record);
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_OFF);
    PublishRecord(record);
    vTaskDelete(nullptr);
}

}  // namespace

static void OtaSendVersion(uint32_t requestId) {
    char line[tagmachine::ota_wire::kMaxLine + 1];
    const size_t size = tagmachine::ota_wire::formatVersion(
        line, sizeof(line), requestId, tagmachineFirmwareVersion,
        tagmachinePartitionVersion, tagmachineBootId);
    if (size) fromSubSerial.write(reinterpret_cast<const uint8_t *>(line), size);
}

static void OtaSendResult(uint32_t requestId, Outcome outcome) {
    char line[tagmachine::ota_wire::kMaxLine + 1];
    const size_t size = tagmachine::ota_wire::formatOutcome(
        line, sizeof(line), requestId, outcome, tagmachineFirmwareVersion,
        tagmachinePartitionVersion, tagmachineBootId);
    if (size) fromSubSerial.write(reinterpret_cast<const uint8_t *>(line), size);
}

void OtaInit() {
    otaEvents = xQueueCreate(4, sizeof(Record));
    if (!otaEvents) abort();

    Preferences preferences;
    if (!preferences.begin(kOtaNamespace, true)) return;
    if (preferences.getBytesLength(kOtaRecordKey) == sizeof(lastRecord)) {
        preferences.getBytes(kOtaRecordKey, &lastRecord, sizeof(lastRecord));
    }
    preferences.end();

    if (!tagmachine::ota_record::valid(lastRecord)) {
        lastRecord = Record{};
        return;
    }

    const Result recovered = tagmachine::ota_record::recover(
        lastRecord, tagmachineFirmwareVersion, tagmachinePartitionVersion,
        tagmachineBootId);
    if (tagmachine::ota_record::pending(lastRecord.result) ||
        recovered != lastRecord.result) {
        lastRecord.result = recovered;
        if (recovered == Result::Updated) {
            lastRecord.targetVersion = tagmachineFirmwareVersion;
        }
        StoreRecord(lastRecord);
    }
}

void OtaReplayResult() {
    if (lastRecord.request && lastRecord.result != Result::None) {
        OtaSendResult(lastRecord.request, WireOutcome(lastRecord.result));
    }
}

void OtaRequest(uint32_t requestId, uint32_t targetVersion) {
    if (otaBusy.load()) {
        OtaSendResult(requestId,
                      requestId == activeRequest.load() &&
                              tagmachine::ota_record::sameRequest(
                                  activeRecord, requestId, targetVersion)
                          ? Outcome::Accepted
                          : Outcome::PeerBusy);
        return;
    }

    if (requestId == lastRecord.request) {
        if (tagmachine::ota_record::sameRequest(lastRecord, requestId,
                                                targetVersion)) {
            OtaReplayResult();
        } else {
            OtaSendResult(requestId, Outcome::Failed);
        }
        return;
    }

    if (!requestId ||
        (targetVersion &&
         !tagmachine::ota_record::validVersion(targetVersion))) return;
    if (!OtaSecretConfigured()) {
        OtaSendResult(requestId, Outcome::Disabled);
        return;
    }

    Record pendingRecord;
    pendingRecord.request = requestId;
    pendingRecord.requestedVersion = targetVersion;
    pendingRecord.targetVersion = targetVersion;
    pendingRecord.oldVersion = tagmachineFirmwareVersion;
    pendingRecord.oldBootId = tagmachineBootId;
    pendingRecord.partitionVersion = tagmachinePartitionVersion;
    pendingRecord.result = Result::Accepted;

    // Accepted is a durable promise. Persist it before the UART acknowledgement
    // so a reset in the following instructions can still reconcile this exact
    // request from version/partition/boot evidence.
    if (!StoreRecord(pendingRecord)) {
        OtaSendResult(requestId, Outcome::Failed);
        return;
    }

    activeRecord = pendingRecord;
    lastRecord = pendingRecord;
    activeRequest.store(requestId);
    otaStartedMs = millis();
    otaBusy.store(true);
    OtaSendResult(requestId, Outcome::Accepted);

    if (xTaskCreate(OtaWorker, "tag-beetle-ota", 12288, nullptr, 1,
                    nullptr) != pdPASS) {
        otaBusy.store(false);
        lastRecord.result = Result::Failed;
        StoreRecord(lastRecord);
        OtaSendResult(requestId, Outcome::Failed);
    }
}

void OtaHandleCommand(const tagmachine::ota_wire::Command &command) {
    switch (command.type) {
        case tagmachine::ota_wire::CommandType::Query:
            OtaSendVersion(command.request);
            OtaReplayResult();
            break;
        case tagmachine::ota_wire::CommandType::Update:
            OtaRequest(command.request, command.targetVersion);
            break;
        default:
            break;
    }
}

void OtaPoll() {
    if (!otaEvents) return;
    Record record;
    while (xQueueReceive(otaEvents, &record, 0) == pdTRUE) {
        lastRecord = record;
        if (!tagmachine::ota_record::pending(record.result)) {
            otaBusy.store(false);
        }
        OtaSendResult(record.request, WireOutcome(record.result));
    }
}

bool OtaBusy() { return otaBusy.load(); }

bool OtaHealthy(uint32_t now) {
    // Do not kill a task while it may be writing flash. Stop feeding the task
    // watchdog after the bounded worker deadline and let the MCU recover.
    return !otaBusy.load() ||
           uint32_t(now - otaStartedMs) < BEETLE_OTA_TIMEOUT_MS;
}
