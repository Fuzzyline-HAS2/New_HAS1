#include "wifi_client.h"
#include "network_policy.h"
#include "chip_report.h"
#include "feedback_config.h"
#include "library_and_pin.h"
#include "secrets.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HAS2_Wifi.h>
#include <SecureOTA.h>
#include <IoTGloveOta.h>
#include <IoTGloveOtaClient.h>
#include "telnet.h"
#include <WiFi.h>
#include <atomic>
#include <errno.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace iotglove {
namespace {
constexpr char kRelease[] = "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/iotglove/";
constexpr uint32_t kPollMs = 1000;
constexpr uint32_t kRetryMs = 5000;
HAS2_Wifi wifi;
QueueHandle_t snapshots, commands, results, locations, batteries, otaRequests;
std::atomic<bool> forceSnapshot{false}, commandBusy{false};
// 2 means no initial sensor sample yet; never retain an edge queue.
std::atomic<uint8_t> physicalChip{2};
ChipReportPolicy chipReport;
std::atomic<NetworkOtaStatus> otaState{NetworkOtaStatus::Idle};
struct Location { char room[32]; uint32_t sampledAt; };
struct Battery { float volts; uint32_t sampledAt; };
struct OtaRequest { uint32_t targetVersion = 0; char sourceCommand[40] = {}; };
int version, partitionVersion;
uint32_t bootId, generation = 0, connectionEpoch = 0;
bool hadValid = false;
ServerSnapshot latest;
char lastDevice[24] = {};
char lastDeviceState[64] = {};
char taggerName[24] = {};
char versionReportedTo[24] = {};
bool resetLatched = false;

bool copyString(JsonVariantConst value, char* out, size_t size) {
  const char* text = value.as<const char*>();
  if (!text || strlen(text) >= size) return false;
  strcpy(out, text);
  return true;
}

bool number(JsonVariantConst value, long min, long max, long& out) {
  if (value.is<long>()) out = value.as<long>();
  else if (value.is<const char*>()) {
    const char* s = value.as<const char*>();
    if (!s || !*s) return false;
    char* end;
    errno = 0;
    out = strtol(s, &end, 10);
    if (errno || *end) return false;
  } else return false;
  return out >= min && out <= max;
}

bool liveName(const char* name) { return ChipReportPolicy::liveDevice(name); }

// ReceiveMine identifies the registration by this board's STA MAC. Its response
// need not contain a known game state; the absolute-write API rechecks MAC/key.
bool readChipIdentity() {
  char name[24];
  if (!wifi.ReceiveMineChecked() ||
      !copyString(my["device_name"], name, sizeof(name)) || !liveName(name)) {
    chipReport.disconnect();
    return false;
  }
  long ready, life;
  chipReport.bind(name, number(my["chip_report_ready"], 0, 1, ready) && ready == 1,
      number(my["life_chip"], 0, 1, life) ? static_cast<int>(life) : -1);
  return true;
}

bool decodeSnapshot(ServerSnapshot& out) {
  char game[24], role[24];
  if (!copyString(my["device_name"], out.deviceName, sizeof(out.deviceName)) || !liveName(out.deviceName) ||
      !copyString(my["game_state"], game, sizeof(game)) ||
      !copyString(my["device_state"], lastDeviceState, sizeof(lastDeviceState)) ||
      !copyString(my["role"], role, sizeof(role))) return false;
  if (!decodeServerStates(game, lastDeviceState, out)) return false;
  if (!strcmp(role, "player")) out.role = Role::Player;
  else if (!strcmp(role, "tagger")) out.role = Role::Tagger;
  else if (!strcmp(role, "ghost") || !strcmp(role, "revival")) out.role = Role::Ghost;
  else if (!strcmp(role, "neutral")) out.role = Role::Neutral;
  else return false;
  long count, seconds, sacrificed, open, life, vibe, brightness;
  if (!number(my["revival_count"], 0, 4, count) || !number(my["revival_time"], 1, 86400, seconds) ||
      !number(my["is_sacrificed"], 0, 1, sacrificed) || !number(my["is_open"], 0, 1, open) ||
      !number(my["life_chip"], 0, 100, life) ||
      // 0/1/3 proximity levels plus operator commands 10..17; anything else still rejects the snapshot.
      !number(my["vibe"], 0, feedback_config::kVibeCommandLast, vibe)) return false;
  out.revivalCount = count; out.stepSeconds = seconds;
  out.sacrificed = sacrificed; out.open = open; out.lifeChip = life;
  out.vibe = vibe;
  // Brightness converts here, as on the other HAS1 devices: server 1..100 maps onto raw
  // 1..255, and anything outside that range falls back to kDefaultBrightness instead of
  // rejecting the whole snapshot.
  out.brightness = number(my["brightness"], 1, 100, brightness)
      ? static_cast<uint8_t>(map(brightness, 1, 100, 1, 255))
      : kDefaultBrightness;
  ota::Command otaCommand;
  const auto parsedOta = ota::parseCommand(lastDeviceState, otaCommand);
  out.updateRequested = parsedOta == ota::ParseResult::Valid;
  if (parsedOta != ota::ParseResult::NotCommand) {
    // Malformed OTA strings stay identifiable but must never become latest.
    strncpy(out.updateCommand, lastDeviceState, sizeof(out.updateCommand) - 1);
  }
  if (out.updateRequested) {
    out.otaTtgoVersion = otaCommand.ttgo.version;
    out.otaBeetleVersion = otaCommand.beetle.version;
  }
  out.capturesAllowed = false;
  taggerName[0] = 0;
  copyString(my["tagger_name"], taggerName, sizeof(taggerName));
  // The API has no game epoch. Use a local generation, invalidated on any loss
  // of synchronization or phase/device change. Never replay across a reconnect.
  const bool connectionChanged = !hadValid || strcmp(lastDevice, out.deviceName);
  if (connectionChanged) ++connectionEpoch;
  out.connectionEpoch = connectionEpoch;
  if (connectionChanged || out.role != latest.role || out.phase != latest.phase ||
      gameMutationsAllowed(out) != gameMutationsAllowed(latest)) ++generation;
  snprintf(out.session, sizeof(out.session), "%08lx-%lu", (unsigned long)bootId, (unsigned long)generation);
  strcpy(lastDevice, out.deviceName);
  out.receivedAtMs = millis(); out.valid = true;
  return true;
}

void publishInvalid() {
  hadValid = false;
  latest.valid = false;
  latest.updateRequested = latest.resetRequested = false;
  latest.vibe = 0;
  xQueueOverwrite(snapshots, &latest);
}

bool refresh() {
  ServerSnapshot fresh;
  if (!readChipIdentity() || !decodeSnapshot(fresh)) { publishInvalid(); return false; }
  // The game can be activate while the tagger still waits for the altar.
  // Resolve the configured tagger instead of inferring altar activation from phase.
  if (fresh.phase == Phase::Active && liveName(taggerName) && wifi.ReceiveChecked(taggerName)) {
    const char* remoteName = tag["device_name"] | "";
    const char* remoteRole = tag["role"] | "";
    const char* remoteState = tag["device_state"] | "";
    fresh.capturesAllowed = !strcmp(remoteName, taggerName) && !strcmp(remoteRole, "tagger") &&
                            !strcmp(remoteState, "activate");
  }
  fresh.receivedAtMs = millis();
  latest = fresh; hadValid = true;
  xQueueOverwrite(snapshots, &latest);
  return true;
}

bool send(const char* field, const String& value) {
  return wifi.SendChecked(latest.deviceName, field, value);
}

void execute(const GameEvent& event) {
  GameResult result{event.sequence, GameResult::Status::Rejected};
  // Read immediately before mutation. Snapshot freshness alone is not a lock,
  // but detects stale queues/phase changes without assuming a server transaction.
  if (!refresh()) result.status = GameResult::Status::Unknown;
  else if (commandAllowed(event, latest)) {
    bool ack = false;
    if (event.kind == GameEvent::Kind::SetCount)
      ack = send("revival_count", String(event.value));
    const bool readBack = refresh();
    result.status = readBack && commandApplied(event, latest) ? GameResult::Status::Success :
        (ack && readBack ? GameResult::Status::Rejected : GameResult::Status::Unknown);
    if (result.status != GameResult::Status::Success)
      remoteConsoleLogf("[glove] command %lu unresolved; no automatic replay\n", (unsigned long)event.sequence);
  }
  // The consumer drains results before snapshots. One command in flight keeps
  // a known order for the remaining revival-count command.
  xQueueOverwrite(results, &result);
}

void observePhysicalChip() {
  const uint8_t present = physicalChip.load();
  if (present <= 1) chipReport.observe(present != 0);
}

void reportChip() {
  observePhysicalChip();
  if (!chipReport.due(millis())) return;
  // Bind immediately before writing, independently of full snapshot decoding.
  if (!readChipIdentity()) { publishInvalid(); return; }
  observePhysicalChip();
  ChipReportPolicy::Request request;
  if (!chipReport.begin(millis(), request)) return;
  wifi.SetGloveChipChecked(request.device, request.present);
  const bool readBack = readChipIdentity();
  long value, ready;
  const char* device = my["device_name"] | "";
  const bool verified = readBack && !strcmp(device, request.device) &&
      number(my["life_chip"], 0, 1, value) && value == (request.present ? 1 : 0) &&
      number(my["chip_report_ready"], 0, 1, ready) && ready == 1;
  // A sensor update may arrive during either HTTP request. The old readback
  // acknowledges only its own value; a newer physical value remains pending.
  observePhysicalChip();
  chipReport.finish(request, verified, millis());
  if (!readBack) publishInvalid();
  forceSnapshot.store(true);  // Read any server-owned role change on the next loop.
  remoteConsoleLogf("[chip] absolute=%u readback=%s\n", request.present ? 1 : 0,
      verified ? "confirmed" : "pending");
}

void checkManagement() {
  if (!hadValid || !wifi.LoopChecked()) return;
  long watchdog;
  if (!number(shift_machine["watchdog"], 0, 1, watchdog)) return;
  if (!watchdog) { resetLatched = false; return; }
  if (!resetLatched && latest.phase != Phase::Active && otaState.load() != NetworkOtaStatus::Running &&
      send("watchdog", "0") && wifi.LoopChecked() &&
      number(shift_machine["watchdog"], 0, 1, watchdog) && watchdog == 0) {
    latest.resetRequested = true;
    resetLatched = true;
    xQueueOverwrite(snapshots, &latest);
  }
}

void completeOtaCommand(const OtaRequest& request) {
  // Another version may have been requested while this update was running.
  // Only acknowledge the exact original server command; USB requests own none.
  if (request.sourceCommand[0] && refresh() &&
      latest.phase != Phase::Active && latest.phase != Phase::Exploration &&
      !strcmp(lastDeviceState, request.sourceCommand)) send("device_state", "setting");
}

void performOta(const OtaRequest& request) {
#ifdef IOTGLOVE_COMPILE_ONLY
  otaState.store(NetworkOtaStatus::Failed);
  return;
#endif
  if (!refresh() || latest.phase == Phase::Active || latest.phase == Phase::Exploration ||
      !HMAC_SECRET[0] || !strncmp(HMAC_SECRET, "REPLACE_WITH_", 13) ||
      strstr(HMAC_SECRET, "PLACEHOLDER") || strstr(HMAC_SECRET, "COMPILE_ONLY")) {
    remoteConsoleLogf("[OTA] TTGO preflight failed (server/phase/key configuration)\n");
    otaState.store(NetworkOtaStatus::Failed); return;
  }
  otaState.store(NetworkOtaStatus::Running);
  remoteConsoleLogf("[OTA] TTGO checking current=%d target=%lu (0=latest) partition=%d\n",
      version, (unsigned long)request.targetVersion, partitionVersion);
  if (request.targetVersion) {
    const auto result = ota::checkPinned("iotglove", request.targetVersion, version,
        partitionVersion, HMAC_SECRET, [request] {
          remoteConsoleLogf("[OTA] TTGO image verified; rebooting\n");
          completeOtaCommand(request);
        });
    if (result == ota::CheckResult::Skipped) {
      completeOtaCommand(request);
      otaState.store(NetworkOtaStatus::Skipped);
    } else otaState.store(NetworkOtaStatus::Failed);
    remoteConsoleLogf("[OTA] TTGO pinned result=%s\n", result == ota::CheckResult::Skipped ? "skipped" : "failed");
    return;
  }
  const String firmware = String(kRelease) + "update.bin";
  const String versionUrl = String(kRelease) + "version.txt";
  const String signature = String(kRelease) + "update.sig";
  SecureOTA ota(firmware.c_str(), versionUrl.c_str(), signature.c_str(), HMAC_SECRET, version);
  // Initial installation uses min_spiffs via USB. No in-place partition changes.
  ota.setOnSkip([request] {
    remoteConsoleLogf("[OTA] TTGO latest skipped\n");
    completeOtaCommand(request); otaState.store(NetworkOtaStatus::Skipped);
  });
  ota.setOnSuccess([request] {
    remoteConsoleLogf("[OTA] TTGO latest image verified; rebooting\n");
    completeOtaCommand(request);
  });
  ota.check();
  if (otaState.load() == NetworkOtaStatus::Running) {
    remoteConsoleLogf("[OTA] TTGO latest check failed\n");
    otaState.store(NetworkOtaStatus::Failed);
  }
}

void worker(void*) {
  uint32_t lastPoll = 0;
  Location location{};
  Battery battery{};
  while (true) {
    if (WiFi.status() != WL_CONNECTED || WiFi.SSID() != "badland_shoot") {
      chipReport.disconnect();
      publishInvalid();
      // Queued means no flash has started. Do not keep the application locked
      // behind an update that may otherwise wait forever in the reconnect loop.
      if (otaState.load() == NetworkOtaStatus::Queued) {
        OtaRequest cancelled;
        xQueueReceive(otaRequests, &cancelled, 0);
        remoteConsoleLogf("[OTA] TTGO queued request cancelled: WiFi disconnected\n");
        otaState.store(NetworkOtaStatus::Failed);
      }
      if (!wifi.TrySetupFixed("badland", "badland_shoot")) { vTaskDelay(pdMS_TO_TICKS(kRetryMs)); continue; }
      versionReportedTo[0] = 0;
      forceSnapshot.store(true);
    }
    if (otaState.load() == NetworkOtaStatus::Queued) {
      OtaRequest request;
      if (xQueueReceive(otaRequests, &request, 0) == pdTRUE) performOta(request);
      else otaState.store(NetworkOtaStatus::Failed);
    }
    GameEvent event;
    if (xQueueReceive(commands, &event, 0) == pdTRUE) execute(event);
    if (forceSnapshot.exchange(false) || uint32_t(millis() - lastPoll) >= kPollMs) {
      if (refresh()) {
        if (strcmp(versionReportedTo, latest.deviceName) && send("esp_version", String(version)))
          strcpy(versionReportedTo, latest.deviceName);
        checkManagement();
      }
      lastPoll = millis();
    }
    reportChip();
    if (hadValid && xQueueReceive(locations, &location, 0) == pdTRUE) {
      // A lost/expired beacon clears the server field rather than leaving a
      // permanent room. Empty is understood by computeVibe as no location.
      const bool fresh = uint32_t(millis() - location.sampledAt) < kLocationFreshMs;
      send("location", fresh ? location.room : "");
    }
    if (hadValid && xQueueReceive(batteries, &battery, 0) == pdTRUE &&
        uint32_t(millis() - battery.sampledAt) < 60000) {
      send("battery_remaining", String(battery.volts, 2));
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
}  // namespace

bool networkBegin(int firmwareVersion, int firmwarePartitionVersion) {
  if (snapshots) return true;
  version = firmwareVersion; partitionVersion = firmwarePartitionVersion; bootId = esp_random();
  wifi.SetDebugPrint(&remoteConsoleWifiPrint());
  snapshots = xQueueCreate(1, sizeof(ServerSnapshot));
  commands = xQueueCreate(1, sizeof(GameEvent));
  results = xQueueCreate(1, sizeof(GameResult));
  locations = xQueueCreate(1, sizeof(Location));
  batteries = xQueueCreate(1, sizeof(Battery));
  otaRequests = xQueueCreate(1, sizeof(OtaRequest));
  if (!snapshots || !commands || !results || !locations || !batteries || !otaRequests) return false;
  return xTaskCreatePinnedToCore(worker, "glove_network", 12288, nullptr, 1, nullptr, 0) == pdPASS;
}

bool networkPoll(ServerSnapshot& snapshot) { return snapshots && xQueueReceive(snapshots, &snapshot, 0) == pdTRUE; }
bool networkPollResult(GameResult& result) {
  if (!results || xQueueReceive(results, &result, 0) != pdTRUE) return false;
  commandBusy.store(false); return true;
}
bool networkSubmit(const GameEvent& event) {
  if (!commands || otaState.load() == NetworkOtaStatus::Queued || otaState.load() == NetworkOtaStatus::Running) return false;
  bool expected = false;
  if (!commandBusy.compare_exchange_strong(expected, true)) return false;
  if (xQueueSend(commands, &event, 0) == pdTRUE) return true;
  commandBusy.store(false); return false;
}
void networkRequestSnapshot() { forceSnapshot.store(true); }
void networkReportChip(bool present) { physicalChip.store(present ? 1 : 0); }
void networkReportLocation(const char* room) {
  if (!locations || !room || strlen(room) >= sizeof(Location::room)) return;
  Location sample{}; strcpy(sample.room, room); sample.sampledAt = millis();
  xQueueOverwrite(locations, &sample);
}
void networkReportBattery(float volts) {
  if (!batteries || !isfinite(volts) || volts <= 0 || volts > 60) return;
  Battery sample{volts, millis()}; xQueueOverwrite(batteries, &sample);
}
bool networkRequestOta(uint32_t targetVersion, const char* sourceCommand) {
  if (!otaRequests || commandBusy.load() || !sourceCommand ||
      strlen(sourceCommand) >= sizeof(OtaRequest::sourceCommand) ||
      targetVersion > INT32_MAX || otaState.load() != NetworkOtaStatus::Idle) return false;
  OtaRequest request;
  request.targetVersion = targetVersion;
  strcpy(request.sourceCommand, sourceCommand);
  // Called by the sole application loop. The worker sees Queued only after the
  // immutable request has been copied, and never consults later server targets.
  if (xQueueSend(otaRequests, &request, 0) != pdTRUE) return false;
  otaState.store(NetworkOtaStatus::Queued);
  return true;
}
NetworkOtaStatus networkOtaStatus() { return otaState.load(); }
void networkClearOtaStatus() {
  auto state = otaState.load();
  if (state == NetworkOtaStatus::Failed || state == NetworkOtaStatus::Skipped)
    otaState.compare_exchange_strong(state, NetworkOtaStatus::Idle);
}
}  // namespace iotglove
