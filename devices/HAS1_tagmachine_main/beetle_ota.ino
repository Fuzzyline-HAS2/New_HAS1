#include <esp_system.h>

namespace {

constexpr uint32_t kBeetleOtaPartitionVersion = 1;
constexpr uint32_t kBeetleDiscoveryTimeoutMs = 12000;
// Beetle stops feeding its 12s watchdog after a 180s worker deadline, so the
// coordinator must still be listening when that recovery boot reports Failed.
constexpr uint32_t kBeetleUpdateTimeoutMs = 210000;
constexpr uint32_t kBeetleCommandRetryMs = 1500;
constexpr uint32_t kBeetleHealthTimeoutMs = 15000;
constexpr uint32_t kServerClearRetryMs = 10000;
constexpr uint32_t kTtgoOnlyRetryMs = 10000;
constexpr uint32_t kAllBoardGateRetryMs = 5000;
constexpr uint8_t kTtgoOnlyMaxChecks = 3;
constexpr uint8_t kBeetleMaxAttempts = 3;
constexpr uint32_t kBeetleRetryMs = 5000;
constexpr char kTtgoOtaNamespace[] = "tag-ttgo-ota";
constexpr char kTtgoOtaRecordKey[] = "pending";

tagmachine::BeetleOtaPeer otaPeers[BEETLE_COUNT];
tagmachine::BeetleOtaSequence otaSequence;
uint32_t lastOtaCommandMs = 0;
bool ttgoOtaCheckStarted = false;
bool ttgoOtaCallbackSeen = false;
bool ttgoOtaCommandCleared = false;
bool ttgoOtaImageCommitted = false;
bool ttgoRecoveryClearPending = false;
bool healthProbePending = false;
uint32_t healthProbeStartedMs = 0;
uint32_t lastHealthProbeMs = 0;
uint32_t healthProbeReadyCount = 0;
bool otaRequested = false;
bool inDoubtLogged = false;
bool cancelAfterCurrent = false;
bool ttgoOnlyRequested = false;
bool ttgoOnlyAttempted = false;
bool ttgoOnlyQuarantined = false;
bool ttgoOnlyExhausted = false;
uint8_t ttgoOnlyCheckCount = 0;
uint32_t fullTtgoTargetVersion = 0;
uint32_t beetleTargetVersion = 0;
uint32_t ttgoOnlyTargetVersion = 0;
bool fullTargetsResolved = false;
bool fullTargetInvalid = false;
bool ttgoOnlyTargetResolved = false;
bool allBoardGateAttempted = false;
bool boardRetryPending = false;
uint8_t boardFailureCount[BEETLE_COUNT] = {};
uint32_t boardRetryDueMs = 0;
uint32_t lastClearAttemptMs = 0;
uint32_t lastTtgoOnlyAttemptMs = 0;
uint32_t lastAllBoardGateMs = 0;
String otaDeviceName;
String otaExpectedCommand;
String ttgoOtaExpectedCommand;

bool StoreTtgoOtaRecord(uint32_t targetVersion, const String &command) {
  const auto record = tagmachine::ttgo_ota_record::make(
      targetVersion, FIRMWARE_VER, PARTITION_VER, command.c_str());
  if (!tagmachine::ttgo_ota_record::valid(record)) return false;
  Preferences preferences;
  if (!preferences.begin(kTtgoOtaNamespace, false)) return false;
  const bool stored = preferences.putBytes(
      kTtgoOtaRecordKey, &record, sizeof(record)) == sizeof(record);
  preferences.end();
  return stored;
}

bool LoadTtgoOtaRecord(tagmachine::ttgo_ota_record::Record &record) {
  Preferences preferences;
  if (!preferences.begin(kTtgoOtaNamespace, true)) return false;
  const bool loaded = preferences.getBytesLength(kTtgoOtaRecordKey) ==
                          sizeof(record) &&
      preferences.getBytes(kTtgoOtaRecordKey, &record, sizeof(record)) ==
          sizeof(record);
  preferences.end();
  return loaded && tagmachine::ttgo_ota_record::valid(record);
}

void ClearTtgoOtaRecord() {
  Preferences preferences;
  if (!preferences.begin(kTtgoOtaNamespace, false)) return;
  preferences.remove(kTtgoOtaRecordKey);
  preferences.end();
}

bool ParseAllBoardCommand(const String &command,
                          uint32_t &ttgoTarget, uint32_t &beetleTarget) {
  // TagMachine TTGO and Beetle version lines are independent; unlike the
  // glove, a one-number shorthand must not accidentally target both with 14.
  if (command != "github" && command.indexOf(':') < 0) return false;
  iotglove::ota::Command parsed;
  if (iotglove::ota::parseCommand(command.c_str(), parsed) !=
      iotglove::ota::ParseResult::Valid) return false;
  ttgoTarget = parsed.ttgo.version;
  beetleTarget = parsed.beetle.version;
  return true;
}

bool ParseTtgoOnlyCommand(const String &command, uint32_t &target) {
  target = 0;
  if (command == "github-ttgo") return true;
  constexpr char kPrefix[] = "github-ttgo@";
  if (!command.startsWith(kPrefix)) return false;
  return iotglove::ota::parseVersion(command.c_str() + sizeof(kPrefix) - 1,
                                     target);
}

bool SafeToStartOta() {
  const String gameState = (String)(const char*)my["game_state"];
  return (gameState == "setting" || gameState == "ready") && !loginDone;
}

bool RefreshAuthoritativeState(String &deviceState, bool requireGameState) {
  if (WiFi.status() != WL_CONNECTED || !otaDeviceName.length()) return false;
  ServiceBeetleLinks();
  if (!has2wifi.ReceiveMineChecked()) return false;
  ServiceBeetleLinks();
  const String refreshedName = (String)(const char*)my["device_name"];
  deviceState = (String)(const char*)my["device_state"];
  const String gameState = (String)(const char*)my["game_state"];
  return refreshedName == otaDeviceName && deviceState.length() &&
         (!requireGameState || gameState.length());
}

enum class OtaGate : uint8_t { Deferred, Safe, Superseded, Unsafe };

OtaGate CheckAllBoardGate() {
  const uint32_t now = millis();
  if (allBoardGateAttempted &&
      static_cast<uint32_t>(now - lastAllBoardGateMs) < kAllBoardGateRetryMs)
    return OtaGate::Deferred;
  allBoardGateAttempted = true;
  lastAllBoardGateMs = now;
  String deviceState;
  if (!RefreshAuthoritativeState(deviceState, true)) return OtaGate::Deferred;
  if (deviceState != otaExpectedCommand) return OtaGate::Superseded;
  return SafeToStartOta() ? OtaGate::Safe : OtaGate::Unsafe;
}

uint32_t TtgoOnlyRetryDelayMs() {
  const uint8_t exponent = ttgoOnlyCheckCount > 2
      ? 2 : (ttgoOnlyCheckCount ? ttgoOnlyCheckCount - 1 : 0);
  return kTtgoOnlyRetryMs << exponent;
}

bool ResolveFullTargets() {
  if (fullTargetsResolved) {
    const bool valid = iotglove::ota::monotonicTarget(
        FIRMWARE_VER, fullTtgoTargetVersion) && beetleTargetVersion;
    fullTargetInvalid = !valid;
    return valid;
  }
  uint32_t ttgoTarget = 0, beetleTarget = 0;
  if (!ParseAllBoardCommand(otaExpectedCommand, ttgoTarget, beetleTarget))
    return false;
  if (!ttgoTarget && !iotglove::ota::fetchLatestTarget(
          "HAS1_tagmachine_main", PARTITION_VER, HMAC_SECRET, ttgoTarget))
    return false;
  if (!beetleTarget && !iotglove::ota::fetchLatestTarget(
          "HAS1_tagmachine_sub", kBeetleOtaPartitionVersion, HMAC_SECRET,
          beetleTarget))
    return false;
  if (!iotglove::ota::monotonicTarget(FIRMWARE_VER, ttgoTarget) ||
      !beetleTarget) {
    fullTargetInvalid = otaExpectedCommand != "github";
    return false;
  }
  fullTtgoTargetVersion = ttgoTarget;
  beetleTargetVersion = beetleTarget;
  fullTargetsResolved = true;
  Serial.printf("[O] T=%lu B=%lu\n", (unsigned long)ttgoTarget,
                (unsigned long)beetleTarget);
  return true;
}

uint32_t NewRequestId() {
  uint32_t value = 0;
  while (value == 0) value = esp_random();
  return value;
}

bool ScheduleBoardRetry(int idx, uint32_t now) {
  if (idx < 0 || idx >= BEETLE_COUNT ||
      boardFailureCount[idx] >= kBeetleMaxAttempts - 1 ||
      !otaSequence.retryCurrent()) return false;
  ++boardFailureCount[idx];
  boardRetryPending = true;
  boardRetryDueMs = now +
      (kBeetleRetryMs << (boardFailureCount[idx] - 1));
  otaPeers[idx] = tagmachine::BeetleOtaPeer{};
  healthProbePending = false;
  inDoubtLogged = false;
  return true;
}

int StageBeetle(tagmachine::BeetleOtaStage stage) {
  switch (stage) {
    case tagmachine::BeetleOtaStage::DiscoverMain:
    case tagmachine::BeetleOtaStage::UpdateMain:
      return BEETLE_MAIN;
    case tagmachine::BeetleOtaStage::DiscoverSub:
    case tagmachine::BeetleOtaStage::UpdateSub:
      return BEETLE_SUB;
    default:
      return -1;
  }
}

bool DiscoveryStage(tagmachine::BeetleOtaStage stage) {
  return stage == tagmachine::BeetleOtaStage::DiscoverMain ||
         stage == tagmachine::BeetleOtaStage::DiscoverSub;
}

bool UpdateStage(tagmachine::BeetleOtaStage stage) {
  return stage == tagmachine::BeetleOtaStage::UpdateMain ||
         stage == tagmachine::BeetleOtaStage::UpdateSub;
}

void SendOtaCommand(int idx, tagmachine::ota_wire::CommandType type) {
  if (idx < 0 || idx >= BEETLE_COUNT || beetleLinks[idx].serial == nullptr) return;
  char line[32];
  const size_t length = tagmachine::ota_wire::formatCommand(
      line, sizeof(line), type, otaPeers[idx].request(),
      type == tagmachine::ota_wire::CommandType::Update
          ? beetleTargetVersion : 0);
  if (length) beetleLinks[idx].serial->write(reinterpret_cast<const uint8_t*>(line), length);
  lastOtaCommandMs = millis();
}

void StartDiscovery(int idx) {
  healthProbePending = false;
  otaPeers[idx] = tagmachine::BeetleOtaPeer{};
  otaPeers[idx].discover(NewRequestId(), millis());
  allBoardGateAttempted = false;
  SendOtaCommand(idx, tagmachine::ota_wire::CommandType::Query);
  Serial.printf("[O] %s Q\n", beetleLinks[idx].name);
}

void SendHealthProbe(int idx) {
  if (idx < 0 || idx >= BEETLE_COUNT || beetleLinks[idx].serial == nullptr) return;
  beetleLinks[idx].serial->println('H');
  lastHealthProbeMs = millis();
}

void QuiesceGameForOta() {
  ptrCurrentMode = WaitFunc;
  GameTimer.deleteTimer(gameTimerId);
  SubSerialTimer.deleteTimer(subSerialTimerId);
  DebuffTimer.deleteTimer(debuffTimerId);
  SubSerialTimerStart = false;
  loginDone = false;
  pendingDeviceState = "";
  pendingDeviceStateApply = false;
  digitalWrite(RELAY_PIN, LOW);
  SubSerialFlush();
  MainSerialFlush();
}

void RestoreRuntimeAfterOta() {
  const String gameState = (String)(const char*)my["game_state"];
  if (gameState == "setting") SettingFunc();
  else if (gameState == "ready") ReadyFunc();
  else if (gameState == "activate") ActivateFunc();
  else ptrCurrentMode = WaitFunc;

  const String deviceState = (String)(const char*)my["device_state"];
  if (deviceState == "lock" || deviceState == "activate" ||
      deviceState == "debuff" || deviceState == "tagger")
    ApplyDeviceState(deviceState);
}

void FinishOtaSequence() {
  const String command = (String)(const char*)my["device_state"];
  otaSequence.reset();
  otaRequested = false;
  inDoubtLogged = false;
  cancelAfterCurrent = false;
  ttgoOtaCheckStarted = false;
  ttgoOtaCallbackSeen = false;
  ttgoOtaCommandCleared = false;
  healthProbePending = false;
  healthProbeReadyCount = 0;
  allBoardGateAttempted = false;
  boardRetryPending = false;
  boardFailureCount[BEETLE_MAIN] = 0;
  boardFailureCount[BEETLE_SUB] = 0;
  fullTtgoTargetVersion = 0;
  beetleTargetVersion = 0;
  fullTargetsResolved = false;
  fullTargetInvalid = false;
  otaExpectedCommand = "";
  ttgoOtaExpectedCommand = "";
  SubSerialFlush();
  MainSerialFlush();
  RestoreRuntimeAfterOta();

  // DataChanged deliberately consumes server edges while a board is flashing.
  // Reconcile the final authoritative command explicitly so a still-owned OTA
  // request cannot become inert merely because `cur` already saw that value.
  if (IsAllBoardOtaCommand(command)) BeginBeetleOtaSequence();
  else if (IsTtgoOnlyOtaCommand(command)) BeginTtgoOnlyOta();
}

void CancelOtaSequence(const char* reason) {
  Serial.printf("[O] C %s\n", reason);
  FinishOtaSequence();
}

void FailOtaSequence(const char* reason) {
  Serial.printf("[O] F %s\n", reason);
  otaSequence.fail();
  lastClearAttemptMs = millis();
  if (ClearGithubOtaState(otaExpectedCommand.c_str())) FinishOtaSequence();
  else Serial.println("[O] L");
}

void StartCurrentBoardUpdate(int idx) {
  if (!otaPeers[idx].begin(millis(), kBeetleOtaPartitionVersion,
                           beetleTargetVersion)) {
    FailOtaSequence("peer");
    return;
  }
  SendOtaCommand(idx, tagmachine::ota_wire::CommandType::Update);
  Serial.printf("[O] %s U\n", beetleLinks[idx].name);
}

void AdvanceCompletedBoard() {
  const int completed = StageBeetle(otaSequence.stage());
  Serial.printf("[O] %s OK\n", beetleLinks[completed].name);
  if (cancelAfterCurrent) {
    CancelOtaSequence("state");
    return;
  }
  const OtaGate gate = CheckAllBoardGate();
  if (gate == OtaGate::Deferred) return;
  if (gate != OtaGate::Safe) {
    CancelOtaSequence("gate");
    return;
  }
  otaSequence.completed();
  allBoardGateAttempted = false;
  if (otaSequence.stage() == tagmachine::BeetleOtaStage::DiscoverSub)
    StartDiscovery(BEETLE_SUB);
}

}  // namespace

static void FinishTtgoOnlyOta();

bool BeetleOtaActive() {
  // External game/UART gates use this as a quarantine predicate. A TTGO-only
  // check that returned without a trustworthy readback remains quarantined too.
  return otaSequence.active() || ttgoOnlyQuarantined;
}

bool IsAllBoardOtaCommand(const String &command) {
  uint32_t ttgoTarget = 0, beetleTarget = 0;
  return ParseAllBoardCommand(command, ttgoTarget, beetleTarget);
}

bool IsExpectedAllBoardOtaCommand(const String &command) {
  return command.length() && command == otaExpectedCommand;
}

bool IsTtgoOnlyOtaCommand(const String &command) {
  uint32_t target = 0;
  return ParseTtgoOnlyCommand(command, target);
}

void BeginBeetleOtaSequence() {
  const String deviceName = (String)(const char*)my["device_name"];
  if (deviceName.length()) otaDeviceName = deviceName;
  if (otaSequence.active()) return;
  const String command = (String)(const char*)my["device_state"];
  uint32_t ttgoTarget = 0, beetleTarget = 0;
  if (!ParseAllBoardCommand(command, ttgoTarget, beetleTarget)) return;
  if (ttgoRecoveryClearPending) return;
  if (otaRequested && command == otaExpectedCommand) return;
  otaExpectedCommand = command;
  fullTtgoTargetVersion = ttgoTarget;
  beetleTargetVersion = beetleTarget;
  fullTargetsResolved = ttgoTarget && beetleTarget;
  fullTargetInvalid = false;
  otaRequested = true;
  allBoardGateAttempted = false;
  Serial.println("[O] QUEUE");
}

static void StartQueuedBeetleOtaSequence() {
  Serial.println("[O] START");
  otaDeviceName = (String)(const char*)my["device_name"];
  QuiesceGameForOta();
  otaPeers[BEETLE_MAIN] = tagmachine::BeetleOtaPeer{};
  otaPeers[BEETLE_SUB] = tagmachine::BeetleOtaPeer{};
  otaSequence.start();
  ttgoOtaCheckStarted = false;
  ttgoOtaCallbackSeen = false;
  ttgoOtaCommandCleared = false;
  healthProbePending = false;
  inDoubtLogged = false;
  cancelAfterCurrent = false;
  allBoardGateAttempted = false;
  boardRetryPending = false;
  boardFailureCount[BEETLE_MAIN] = 0;
  boardFailureCount[BEETLE_SUB] = 0;
  StartDiscovery(BEETLE_MAIN);
}

void NoteBeetleOtaCommandChanged() {
  // Never interrupt a board that may be writing flash. The observation is
  // sticky so even a transient server/runtime change prevents the next board
  // (or TTGO) from starting once the current board supplies terminal proof.
  if (otaSequence.active()) cancelAfterCurrent = true;
  if (ttgoOnlyQuarantined &&
      (String)(const char*)my["device_state"] != ttgoOtaExpectedCommand)
    FinishTtgoOnlyOta();
}

void NoteBeetleOtaRuntimeUnsafe() {
  if (otaSequence.active() &&
      otaSequence.stage() != tagmachine::BeetleOtaStage::Failed)
    cancelAfterCurrent = true;
  if (ttgoOnlyQuarantined) {
    SubSerialFlush();
    MainSerialFlush();
    ttgoOnlyQuarantined = false;
    RestoreRuntimeAfterOta();
  }
}

void HandleBeetleOtaResponse(
    int idx, const tagmachine::ota_wire::Response &response) {
  if (idx < 0 || idx >= BEETLE_COUNT || !otaSequence.active()) return;
  tagmachine::BeetleOtaPeer &peer = otaPeers[idx];
  if (response.type == tagmachine::ota_wire::ResponseType::Version) {
    if (peer.receiveVersion(response, millis())) {
      Serial.printf("[O] %s V%lu P%lu B%lu\n", beetleLinks[idx].name,
                    (unsigned long)response.firmware,
                    (unsigned long)response.partition,
                    (unsigned long)response.boot);
    }
  } else if (response.type == tagmachine::ota_wire::ResponseType::Outcome) {
    if (peer.receiveOutcome(response, millis())) {
      Serial.printf("[O] %s R%c V%lu\n", beetleLinks[idx].name,
                    static_cast<char>(response.outcome),
                    (unsigned long)response.firmware);
    }
  }
}

bool ClearGithubOtaState(const char *expectedCommand) {
  // A pending TTGO record is recovered before the normal polling loop has
  // necessarily populated `my`.  Re-latch the name on every retry so a boot
  // with temporarily unavailable server data cannot wedge OTA recovery.
  if (!otaDeviceName.length()) {
    const String deviceName = (String)(const char*)my["device_name"];
    if (deviceName.length()) otaDeviceName = deviceName;
  }
  if (!expectedCommand || !expectedCommand[0] || !otaDeviceName.length()) {
    Serial.println("[O] CLR-NAME");
    return false;
  }
  for (uint8_t attempt = 0; attempt < 5; ++attempt) {
    String refreshedState;
    if (RefreshAuthoritativeState(refreshedState, false)) {
      // Do not overwrite a newer operator command that arrived while an OTA
      // check was blocking. A different authoritative value supersedes the
      // command owned by this transaction and needs no clear write.
      if (refreshedState != expectedCommand) {
        Serial.println("[O] CLR-SUPERSEDED");
        return true;
      }

      if (!has2wifi.SendChecked(otaDeviceName, "device_state", "setting")) {
        ServiceBeetleLinks();
        delay(300);
        continue;
      }
      delay(300);
      if (RefreshAuthoritativeState(refreshedState, false) &&
          refreshedState != expectedCommand) {
        Serial.printf("[O] CLR %u\n", attempt + 1);
        return true;
      }
    }
    ServiceBeetleLinks();
    delay(300);
  }
  Serial.println("[O] CLR-FAIL");
  return false;
}

void RecoverPendingTtgoOta() {
  tagmachine::ttgo_ota_record::Record record;
  if (!LoadTtgoOtaRecord(record)) return;
  otaDeviceName = (String)(const char*)my["device_name"];
  if (!tagmachine::ttgo_ota_record::bootProvesInstalled(
          record, FIRMWARE_VER, PARTITION_VER)) {
    Serial.println("[O] BOOT-FAIL");
    ClearTtgoOtaRecord();
    return;
  }
  ttgoOtaExpectedCommand = record.command;
  Serial.println("[O] BOOT-OK");
  if (ClearGithubOtaState(record.command)) {
    ClearTtgoOtaRecord();
    ttgoOtaExpectedCommand = "";
  } else {
    ttgoRecoveryClearPending = true;
    lastClearAttemptMs = millis();
  }
}

static void FinishTtgoRecoveryClear() {
  const String command = (String)(const char*)my["device_state"];
  ttgoRecoveryClearPending = false;
  ClearTtgoOtaRecord();
  ttgoOtaExpectedCommand = "";
  // This is a clear-only retry after the new TTGO image has already booted.
  // Runtime was never quarantined, so flushing UART or re-running a mode
  // initializer here could interrupt an in-progress login/game transaction.
  if (IsAllBoardOtaCommand(command)) BeginBeetleOtaSequence();
  else if (IsTtgoOnlyOtaCommand(command)) BeginTtgoOnlyOta();
}

bool CompleteTtgoOtaCommand() {
  ttgoOtaCallbackSeen = true;
  ttgoOtaCommandCleared =
      ClearGithubOtaState(ttgoOtaExpectedCommand.c_str());
  if (ttgoOtaCommandCleared) ClearTtgoOtaRecord();
  return ttgoOtaCommandCleared;
}

static bool RunSignedTtgoCheck(uint32_t targetVersion) {
  if (!iotglove::ota::monotonicTarget(FIRMWARE_VER, targetVersion)) {
    Serial.println("[O] TTGO-TARGET");
    return false;
  }
  const bool updateRequired = targetVersion > FIRMWARE_VER;
  if (updateRequired &&
      !StoreTtgoOtaRecord(targetVersion, ttgoOtaExpectedCommand)) {
    Serial.println("[O] NVS");
    return false;
  }
  ttgoOtaImageCommitted = false;
  const auto result = iotglove::ota::checkPinned(
      "HAS1_tagmachine_main", targetVersion, FIRMWARE_VER, PARTITION_VER,
      HMAC_SECRET, []() { ttgoOtaImageCommitted = true; });
  if (result == iotglove::ota::CheckResult::Skipped) {
    CompleteTtgoOtaCommand();
  } else if (!ttgoOtaImageCommitted) {
    // checkPinned commits only after complete image HMAC verification. A
    // normal failed return therefore cannot be recovered as an installation.
    ClearTtgoOtaRecord();
  } else {
    // ESP.restart() should not return. Keep the durable proof and retry the
    // restart rather than acknowledging the command from the old image.
    Serial.println("[O] REBOOT");
    delay(100);
    ESP.restart();
  }
  return ttgoOtaCallbackSeen;
}

static void ResetTtgoOnlyState() {
  ttgoOnlyRequested = false;
  ttgoOnlyAttempted = false;
  ttgoOnlyQuarantined = false;
  ttgoOnlyExhausted = false;
  ttgoOnlyCheckCount = 0;
  ttgoOnlyTargetVersion = 0;
  ttgoOnlyTargetResolved = false;
  ttgoOtaExpectedCommand = "";
}

static void FinishTtgoOnlyOta() {
  const String command = (String)(const char*)my["device_state"];
  const bool restoreNeeded = ttgoOnlyQuarantined;
  ResetTtgoOnlyState();
  if (restoreNeeded) {
    SubSerialFlush();
    MainSerialFlush();
    RestoreRuntimeAfterOta();
  }
  // The polling callback consumes server edges while quarantine is active.
  // Explicitly hand off any newer OTA command observed by the final readback.
  if (IsAllBoardOtaCommand(command)) BeginBeetleOtaSequence();
  else if (IsTtgoOnlyOtaCommand(command)) BeginTtgoOnlyOta();
}

void BeginTtgoOnlyOta() {
  const String command = (String)(const char*)my["device_state"];
  uint32_t targetVersion = 0;
  if (!ParseTtgoOnlyCommand(command, targetVersion)) return;
  if (ttgoRecoveryClearPending) return;
  if (ttgoOnlyRequested && command == ttgoOtaExpectedCommand) return;
  if (ttgoOnlyRequested) CancelTtgoOnlyOta();
  otaDeviceName = (String)(const char*)my["device_name"];
  if (!otaDeviceName.length()) return;
  ttgoOnlyRequested = true;
  ttgoOnlyAttempted = false;
  ttgoOnlyQuarantined = false;
  ttgoOnlyExhausted = false;
  ttgoOnlyCheckCount = 0;
  ttgoOnlyTargetVersion = targetVersion;
  ttgoOnlyTargetResolved = targetVersion != 0;
  ttgoOtaExpectedCommand = command;
  Serial.println("[O] TQ");
}

bool CancelTtgoOnlyOta() {
  if (!ttgoOnlyRequested) return false;
  const bool restoreNeeded = ttgoOnlyQuarantined;
  ResetTtgoOnlyState();
  Serial.println("[O] TC");
  if (restoreNeeded) {
    SubSerialFlush();
    MainSerialFlush();
    RestoreRuntimeAfterOta();
  }
  return restoreNeeded;
}

static void RunTtgoOnlyOta() {
  // Count every network preflight as an attempt so a failed authoritative read
  // also observes the retry backoff instead of hammering HTTP every loop.
  ttgoOnlyAttempted = true;
  lastTtgoOnlyAttemptMs = millis();
  String refreshedState;
  if (!RefreshAuthoritativeState(refreshedState, true)) {
    return;
  }
  if (refreshedState != ttgoOtaExpectedCommand) {
    FinishTtgoOnlyOta();
    return;
  }
  // An explicit downgrade is a terminal operator error, not a transient OTA
  // failure.  Clear it without entering quarantine or consuming three signed
  // check attempts.  Same-version targets remain valid no-op installations.
  if (ttgoOnlyTargetResolved &&
      !iotglove::ota::monotonicTarget(FIRMWARE_VER,
                                      ttgoOnlyTargetVersion)) {
    Serial.println("[O] TTGO-TARGET");
    if (ClearGithubOtaState(ttgoOtaExpectedCommand.c_str()))
      FinishTtgoOnlyOta();
    return;
  }
  if (!SafeToStartOta()) {
    // A real game phase always wins over a pre-commit retry. Keep the request
    // pending, but release the runtime until a later authoritative safe phase.
    if (ttgoOnlyQuarantined) {
      SubSerialFlush();
      MainSerialFlush();
      ttgoOnlyQuarantined = false;
      RestoreRuntimeAfterOta();
    }
    return;
  }

  if (ttgoOnlyExhausted) {
    if (!ttgoOnlyQuarantined) {
      QuiesceGameForOta();
      ttgoOnlyQuarantined = true;
    }
    if (ClearGithubOtaState(ttgoOtaExpectedCommand.c_str()))
      FinishTtgoOnlyOta();
    else digitalWrite(RELAY_PIN, LOW);
    return;
  }

  if (!ttgoOnlyTargetResolved) {
    uint32_t targetVersion = 0;
    if (!iotglove::ota::fetchLatestTarget(
            "HAS1_tagmachine_main", PARTITION_VER, HMAC_SECRET,
            targetVersion) ||
        !iotglove::ota::monotonicTarget(FIRMWARE_VER, targetVersion)) {
      Serial.println("[O] TF-TARGET");
      return;
    }
    ttgoOnlyTargetVersion = targetVersion;
    ttgoOnlyTargetResolved = true;
    // The signed lookup is blocking. Recheck command identity and game phase
    // immediately before any OTA partition write.
    if (!RefreshAuthoritativeState(refreshedState, true)) return;
    if (refreshedState != ttgoOtaExpectedCommand) {
      FinishTtgoOnlyOta();
      return;
    }
    if (!SafeToStartOta()) return;
  }

  Serial.println("[O] TS");
  if (!ttgoOnlyQuarantined) QuiesceGameForOta();
  ttgoOnlyQuarantined = true;
  ++ttgoOnlyCheckCount;
  ttgoOtaCallbackSeen = false;
  ttgoOtaCommandCleared = false;
  RunSignedTtgoCheck(ttgoOnlyTargetVersion);
  if (!ttgoOtaCallbackSeen)
    Serial.println("[O] TF");
  else if (!ttgoOtaCommandCleared)
    Serial.println("[O] TCLEAR");
  if ((ttgoOtaCallbackSeen && !ttgoOtaCommandCleared) ||
      (!ttgoOtaCallbackSeen && ttgoOnlyCheckCount >= kTtgoOnlyMaxChecks))
    ttgoOnlyExhausted = true;

  // The signed check may block long enough for the game/device command to change.
  // Never restore from the stale pre-check document. Also discard any tags or
  // relay pulse queued while the synchronous check was running.
  SubSerialFlush();
  MainSerialFlush();
  lastTtgoOnlyAttemptMs = millis();
  if (!RefreshAuthoritativeState(refreshedState, true)) {
    digitalWrite(RELAY_PIN, LOW);
    return;
  }
  if (refreshedState != ttgoOtaExpectedCommand) {
    FinishTtgoOnlyOta();
    return;
  }
  if (!SafeToStartOta()) {
    SubSerialFlush();
    MainSerialFlush();
    ttgoOnlyQuarantined = false;
    RestoreRuntimeAfterOta();
    return;
  }
  if (ttgoOnlyExhausted &&
      ClearGithubOtaState(ttgoOtaExpectedCommand.c_str())) {
    FinishTtgoOnlyOta();
    return;
  }
  digitalWrite(RELAY_PIN, LOW);
}

void ServiceBeetleOtaSequence() {
  if (!otaSequence.active()) {
    if (ttgoRecoveryClearPending) {
      const uint32_t now = millis();
      if (static_cast<uint32_t>(now - lastClearAttemptMs) >=
          kServerClearRetryMs) {
        lastClearAttemptMs = now;
        if (ClearGithubOtaState(ttgoOtaExpectedCommand.c_str()))
          FinishTtgoRecoveryClear();
      }
      return;
    }
    if (ttgoOnlyRequested) {
      const uint32_t now = millis();
      if (!ttgoOnlyAttempted ||
          static_cast<uint32_t>(now - lastTtgoOnlyAttemptMs) >=
              TtgoOnlyRetryDelayMs()) RunTtgoOnlyOta();
      else if (ttgoOnlyQuarantined) digitalWrite(RELAY_PIN, LOW);
      return;
    }
    if (!otaRequested) return;
    const OtaGate gate = CheckAllBoardGate();
    if (gate == OtaGate::Superseded) {
      otaRequested = false;
      Serial.println("[O] QC");
      return;
    }
    if (gate != OtaGate::Safe) return;
    if (!ResolveFullTargets()) {
      Serial.println("[O] QF");
      if (fullTargetInvalid &&
          ClearGithubOtaState(otaExpectedCommand.c_str()))
        FinishOtaSequence();
      return;
    }
    // Target discovery is blocking; obtain a second authoritative phase and
    // exact-command check immediately before quiescing the game.
    allBoardGateAttempted = false;
    const OtaGate finalGate = CheckAllBoardGate();
    if (finalGate == OtaGate::Superseded) {
      otaRequested = false;
      Serial.println("[O] QR");
      return;
    }
    if (finalGate != OtaGate::Safe) return;
    StartQueuedBeetleOtaSequence();
  }

  const uint32_t now = millis();
  const tagmachine::BeetleOtaStage stage = otaSequence.stage();
  const int idx = StageBeetle(stage);
  // No game/server callback may reopen the door while any OTA state is active.
  digitalWrite(RELAY_PIN, LOW);

  if (stage == tagmachine::BeetleOtaStage::Failed) {
    if (static_cast<uint32_t>(now - lastClearAttemptMs) >=
               kServerClearRetryMs) {
      lastClearAttemptMs = now;
      if (ClearGithubOtaState(otaExpectedCommand.c_str())) FinishOtaSequence();
    }
    return;
  }

  if (DiscoveryStage(stage)) {
    if (cancelAfterCurrent) {
      CancelOtaSequence("state");
      return;
    }
    if (boardRetryPending) {
      if (static_cast<int32_t>(now - boardRetryDueMs) < 0) return;
      boardRetryPending = false;
      StartDiscovery(idx);
      return;
    }
    otaPeers[idx].tick(now, kBeetleDiscoveryTimeoutMs, kBeetleUpdateTimeoutMs);
    if (otaPeers[idx].state() == tagmachine::BeetleOtaState::Capable) {
      const OtaGate gate = CheckAllBoardGate();
      if (gate == OtaGate::Deferred) return;
      if (gate != OtaGate::Safe) {
        CancelOtaSequence("gate");
        return;
      }
      otaSequence.discovered();
      StartCurrentBoardUpdate(idx);
      return;
    }
    if (otaPeers[idx].state() == tagmachine::BeetleOtaState::Failed) {
      if (ScheduleBoardRetry(idx, now)) {
        Serial.printf("[O] %s RQ\n", beetleLinks[idx].name);
        return;
      }
      FailOtaSequence("query");
      return;
    }
    if (static_cast<uint32_t>(now - lastOtaCommandMs) >= kBeetleCommandRetryMs)
      SendOtaCommand(idx, tagmachine::ota_wire::CommandType::Query);
    return;
  }

  if (UpdateStage(stage)) {
    otaPeers[idx].tick(now, kBeetleDiscoveryTimeoutMs, kBeetleUpdateTimeoutMs);
    if (otaPeers[idx].state() == tagmachine::BeetleOtaState::Ready) {
      if (!healthProbePending) {
        healthProbePending = true;
        healthProbeStartedMs = now;
        healthProbeReadyCount = beetleLinks[idx].readyCount;
        allBoardGateAttempted = false;
        SendHealthProbe(idx);
        Serial.printf("[O] %s RH\n", beetleLinks[idx].name);
        return;
      }
      const bool freshReady = tagmachine::freshReadyProof(
          beetleLinks[idx].rfidReady, healthProbeReadyCount,
          beetleLinks[idx].readyCount);
      if (freshReady) {
        healthProbePending = false;
        AdvanceCompletedBoard();
        return;
      }
      if (static_cast<uint32_t>(now - healthProbeStartedMs) >= kBeetleHealthTimeoutMs) {
        FailOtaSequence("rfid");
        return;
      }
      if (static_cast<uint32_t>(now - lastHealthProbeMs) >= kBeetleCommandRetryMs)
        SendHealthProbe(idx);
      return;
    }
    if (otaPeers[idx].state() == tagmachine::BeetleOtaState::Failed) {
      if (ScheduleBoardRetry(idx, now)) {
        Serial.printf("[O] %s RU\n", beetleLinks[idx].name);
        return;
      }
      FailOtaSequence("peer");
      return;
    }
    if (otaPeers[idx].state() == tagmachine::BeetleOtaState::InDoubt) {
      if (!inDoubtLogged) {
        inDoubtLogged = true;
        Serial.printf("[O] %s ?\n", beetleLinks[idx].name);
      }
      // A normal server-state change is not permission to unlock while flash
      // outcome is unknown. Keep querying under quarantine until this request
      // produces a terminal, reboot-bound result.
    }
    if (static_cast<uint32_t>(now - lastOtaCommandMs) >= kBeetleCommandRetryMs) {
      // Until Accepted/Flashing is observed, repeat the idempotent U in case
      // the first command was lost. Afterwards Q supplies progress replay and
      // the required post-reboot RV proof.
      const tagmachine::ota_wire::CommandType command =
          otaPeers[idx].progressSeen()
              ? tagmachine::ota_wire::CommandType::Query
              : tagmachine::ota_wire::CommandType::Update;
      SendOtaCommand(idx, command);
    }
    return;
  }

  if (stage == tagmachine::BeetleOtaStage::ReadyForTtgo && !ttgoOtaCheckStarted) {
    if (cancelAfterCurrent) {
      CancelOtaSequence("state");
      return;
    }
    const OtaGate gate = CheckAllBoardGate();
    if (gate == OtaGate::Deferred) return;
    if (gate != OtaGate::Safe) {
      CancelOtaSequence("gate");
      return;
    }
    ttgoOtaCheckStarted = true;
    ttgoOtaCallbackSeen = false;
    ttgoOtaCommandCleared = false;
    ttgoOtaExpectedCommand = otaExpectedCommand;
    Serial.println("[O] TU");
    RunSignedTtgoCheck(fullTtgoTargetVersion);
    if (!ttgoOtaCallbackSeen) {
      FailOtaSequence("ttgo");
      return;
    }
    if (!ttgoOtaCommandCleared) {
      otaSequence.fail();
      lastClearAttemptMs = now;
      Serial.println("[O] TCLEAR");
      return;
    }
    FinishOtaSequence();
  }
}
