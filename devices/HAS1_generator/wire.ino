// All four connectors share one observed-count filter in every active stage.
// 100 ms samples, 1 s stability; gaps over 250 ms restart qualification.
static const uint8_t WIRE_PINS[4] = { WIRE_PIN_1, WIRE_PIN_2, WIRE_PIN_3, WIRE_PIN_4 };
static GeneratorWireState wireState;
static portMUX_TYPE wireStateMux = portMUX_INITIALIZER_UNLOCKED;
static bool wireSamplingEnabled = false; // Access only under wireStateMux.
static StaticTask_t wireSamplerControl;
static StackType_t wireSamplerStack[2048];
static TaskHandle_t wireSamplerTask = nullptr;
static int wireLastPresented = -1; // Main-task presentation, not an event queue.
static bool wireEnabled = false;
static bool wireContextValid = false;
static bool wireRetryPending = false;
static bool wireFenceNeeded = true; // Fence requests left pending by a previous ESP boot.
static uint32_t wireLastAttempt = 0;
static GeneratorWireSnapshot wireContext;
static char wireObservedGame[32] = {};
static char wireObservedDevice[32] = {};
static char wireObservedName[32] = {};

static bool WireTextEquals(const char *a, const char *b) { return a && b && strcmp(a,b) == 0; }
static bool WireMonitoredState(const char *state) {
    return WireTextEquals(state,"activate") || WireTextEquals(state,"battery_max") ||
           WireTextEquals(state,"starter_finish") || WireTextEquals(state,"repaired") ||
           WireTextEquals(state,"repaired_all") || WireTextEquals(state,"tagger");
}

static GeneratorWireState WireSnapshot() {
    portENTER_CRITICAL(&wireStateMux);
    const GeneratorWireState snapshot = wireState;
    portEXIT_CRITICAL(&wireStateMux);
    return snapshot;
}
static void WireInvalidateAck() {
    portENTER_CRITICAL(&wireStateMux);
    wireState.invalidateAck();
    portEXIT_CRITICAL(&wireStateMux);
}
static bool WireAcknowledge(int count, uint32_t generation) {
    portENTER_CRITICAL(&wireStateMux);
    const bool accepted = wireState.acknowledge(count, generation);
    portEXIT_CRITICAL(&wireStateMux);
    return accepted;
}
// Called only by the input task. No JSON, logging, audio or network work here.
static void WireSamplerTick() {
    portENTER_CRITICAL(&wireStateMux);
    const bool enabled = wireSamplingEnabled;
    const uint32_t generation = wireState.generation();
    portEXIT_CRITICAL(&wireStateMux);
    if (!enabled) return;
    const int count = WireCountPlugged();
    const uint32_t observedAt = millis();
    portENTER_CRITICAL(&wireStateMux);
    if (wireSamplingEnabled && wireState.generation() == generation) wireState.sample(observedAt,count);
    portEXIT_CRITICAL(&wireStateMux);
}
static void WireSamplerTaskMain(void *) {
    for (;;) {
        WireSamplerTick();
        // No catch-up samples: use the actual GPIO observation time each turn.
        vTaskDelay(pdMS_TO_TICKS(GeneratorWireState::SampleMs));
    }
}
void WireInit() {
    for (int i = 0; i < 4; ++i) pinMode(WIRE_PINS[i], INPUT_PULLUP);
    if (!wireSamplerTask) {
        wireSamplerTask = xTaskCreateStaticPinnedToCore(WireSamplerTaskMain,"wire-input",
            sizeof(wireSamplerStack),nullptr,2,wireSamplerStack,&wireSamplerControl,tskNO_AFFINITY);
        if (!wireSamplerTask) Serial.println("[Wire] input task creation failed; completion disabled");
    }
}
int WireCountPlugged() {
    int count = 0;
    for (int i = 0; i < 4; ++i) if (digitalRead(WIRE_PINS[i]) == LOW) ++count;
    return count;
}
int WireDisplayCount() { const int count = WireSnapshot().stable(); return count < 0 ? 0 : count; }

void WireResetTracking() {
    portENTER_CRITICAL(&wireStateMux);
    wireState.reset();
    wireSamplingEnabled = wireEnabled;
    portEXIT_CRITICAL(&wireStateMux);
    wireLastPresented = -1;
    Mp3BatteryCancel();
    wireContextValid = false;
    wireFenceNeeded = true; // Re-entry/identity changes must fence any earlier in-flight write.
    batteryFinishDone = false;
    batteryFinishAudioPlayed = false;
    // Never publish raw input or finish while (re)entering a charge cycle.
    Serial.println("[Wire] reset: waiting for 1 s stable inputs and server acknowledgement");
}

void WireObserveServerSnapshot() {
    const char *game = my["game_state"];
    const char *state = my["device_state"];
    const char *name = my["device_name"];
    const bool enabled = WireTextEquals(game,"activate") && WireMonitoredState(state);
    const bool enteringCharge = enabled && WireTextEquals(state,"activate") &&
        !WireTextEquals(wireObservedDevice,"activate");
    const bool newIdentity = name && !WireTextEquals(name,wireObservedName);
    const bool changedServerState = !WireTextEquals(game,wireObservedGame) ||
        !WireTextEquals(state,wireObservedDevice);
    if (newIdentity) wireContext = GeneratorWireSnapshot();
    const bool resetInputs = enabled != wireEnabled || enteringCharge || newIdentity;
    wireEnabled = enabled;
    if (resetInputs) WireResetTracking();
    // State writes also consume CAS revisions. Refresh that context without
    // replaying completion audio or resetting a tagger/starter session.
    if (changedServerState) wireContextValid = false;
    const GeneratorWireState observed = WireSnapshot();
    if (wireContextValid && observed.acknowledged() &&
        ((int)my["battery_pack"] != observed.acknowledgedCount() ||
         (int)my["max_battery_pack"] != wireContext.maximum)) {
        WireInvalidateAck(); wireContextValid = false;
    }
    snprintf(wireObservedGame,sizeof(wireObservedGame),"%s",game ? game : "");
    snprintf(wireObservedDevice,sizeof(wireObservedDevice),"%s",state ? state : "");
    snprintf(wireObservedName,sizeof(wireObservedName),"%s",name ? name : "");
}

// Main consumes only the latest state; it does not replay missed transitions.
void WireSampleInputs(bool) {
    if (!wireEnabled) return;
    const GeneratorWireState snapshot = WireSnapshot();
    if (!snapshot.qualified(millis()) || snapshot.stable() == wireLastPresented) return;
    const int previous = wireLastPresented;
    wireLastPresented = snapshot.stable();
    Serial.printf("[Wire] stable=%d previous=%d maximum=%d\n", snapshot.stable(), previous, (int)my["max_battery_pack"]);
    if (ptrCurrentMode == WirePollMain) {
        BatteryPackSend();
        if (!batteryFinishAudioPlayed && previous >= 0 && snapshot.stable() > previous) Mp3PlayLargeFolder(1,7);
    }
}

static bool WireSnapshotCanProgress(const GeneratorWireState &snapshot) {
    return wireSamplerTask && wireEnabled && wireContextValid && WireTextEquals(my["game_state"],"activate") &&
        wireContext.maximum == (int)my["max_battery_pack"] &&
        snapshot.ready(millis(), (int)my["max_battery_pack"]);
}
bool WireCanProgress() { return WireSnapshotCanProgress(WireSnapshot()); }
bool WireReadyForCompletion() {
    // Do not sample or refresh the input task's heartbeat from main. A stopped
    // sampler must remain unqualified even if main can still read the pins.
    const GeneratorWireState before = WireSnapshot();
    const int raw = WireCountPlugged();
    portENTER_CRITICAL(&wireStateMux);
    if (wireState.generation() == before.generation() && raw != wireState.raw()) wireState.invalidateObservation();
    const GeneratorWireState after = wireState;
    portEXIT_CRITICAL(&wireStateMux);
    return before.generation() == after.generation() && raw == after.raw() &&
        raw == after.stable() && WireSnapshotCanProgress(after);
}

static bool WireAdoptContext(const GeneratorWireSnapshot &snapshot) {
    const bool changedEpoch = wireContext.epoch[0] && strcmp(wireContext.epoch,snapshot.epoch) != 0;
    const bool stateMismatch = !WireTextEquals(snapshot.gameState,my["game_state"]) ||
        !WireTextEquals(snapshot.deviceState,my["device_state"]) ||
        snapshot.maximum != (int)my["max_battery_pack"];
    if (!changedEpoch && wireContext.epoch[0] && snapshot.revision < wireContext.revision) {
        WireInvalidateAck(); wireContextValid = false;
        Serial.println("[WireSync] stale revision rejected");
        return false;
    }
    wireContext = snapshot;
    if (changedEpoch || stateMismatch) {
        const bool resetCycle = changedEpoch || !WireTextEquals(snapshot.gameState,my["game_state"]) ||
            (WireTextEquals(snapshot.deviceState,"activate") && !WireTextEquals(my["device_state"],"activate"));
        if (resetCycle) WireResetTracking();
        else { WireInvalidateAck(); wireContextValid = false; }
        // The CAS reply is a partial object, never replace the full device JSON.
        // A failed legacy refresh leaves the acknowledgement invalid; the next
        // bounded retry fetches and checks the CAS context again.
        has2wifi.ReceiveMine();
        if (resetCycle) receiveMineOn = false;
        DataChanged();
        if (changedEpoch && WireTextEquals(my["game_state"],snapshot.gameState) &&
            WireTextEquals(my["device_state"],snapshot.deviceState) &&
            WireTextEquals(snapshot.gameState,"activate") &&
            (WireTextEquals(snapshot.deviceState,"activate") ||
             WireTextEquals(snapshot.deviceState,"battery_max") ||
             WireTextEquals(snapshot.deviceState,"starter_finish"))) ActivateFunc();
        Serial.println("[WireSync] server context changed; full state refresh requested");
        return false;
    }
    wireContextValid = true;
    return true;
}

void WireServiceLoop() {
    WireObserveServerSnapshot();
    WireSampleInputs(false);
    GeneratorWireState observed = WireSnapshot();
    if (!wireSamplerTask || !wireEnabled || !observed.qualified(millis()) ||
        (wireContextValid && observed.synced() && !wireFenceNeeded)) return;
    // Fresh stable values are sent immediately. Only failures are backed off.
    if (wireRetryPending && uint32_t(millis()-wireLastAttempt) < 2000) return;
    char name[32];
    snprintf(name,sizeof(name),"%s",wireObservedName);
    GeneratorWireSnapshot response;
    if (!wireContextValid) {
        const uint32_t generation = observed.generation();
        const GeneratorWireResult result = WireHttpRequest(false,name,0,"",0,response);
        wireLastAttempt = millis();
        if (result != GeneratorWireResult::Ok || WireSnapshot().generation() != generation || !WireAdoptContext(response)) {
            WireInvalidateAck(); wireContextValid = false; wireRetryPending = true; return;
        }
        observed = WireSnapshot();
        if (response.count == observed.stable() && !wireFenceNeeded) {
            if (!WireAcknowledge(response.count,generation)) {
                wireContextValid = false; wireRetryPending = true; return;
            }
            wireRetryPending = false;
            my["battery_pack"] = response.count; SyncBatteryPackCur();
            Serial.printf("[WireSync] confirmed=%d revision=%lu (read)\n",response.count,(unsigned long)response.revision);
            return;
        }
        wireRetryPending = false;
    }
    observed = WireSnapshot();
    if (!observed.qualified(millis())) return;
    const int sending = observed.stable();
    const uint32_t generation = observed.generation();
    const uint32_t expectedRevision = wireContext.revision;
    char expectedEpoch[37]; memcpy(expectedEpoch,wireContext.epoch,sizeof(expectedEpoch));
    // Uncertain writes (including a previous boot) still require a CAS fence.
    wireFenceNeeded = true;
    const GeneratorWireResult result = WireHttpRequest(true,name,sending,expectedEpoch,expectedRevision,response);
    wireLastAttempt = millis();
    if (result != GeneratorWireResult::Ok || strcmp(response.epoch,expectedEpoch) != 0 ||
        response.count != sending || response.revision <= expectedRevision ||
        WireSnapshot().generation() != generation || !WireAdoptContext(response)) {
        WireInvalidateAck(); wireContextValid = false; wireRetryPending = true;
        Serial.println("[WireSync] unconfirmed; retry current stable count after resync");
        return;
    }
    if (WireAcknowledge(response.count,generation)) {
        wireFenceNeeded = false; wireRetryPending = false;
        my["battery_pack"] = response.count; SyncBatteryPackCur();
        Serial.printf("[WireSync] confirmed=%d revision=%lu (CAS)\n",response.count,(unsigned long)response.revision);
    } else {
        wireContextValid = false; wireRetryPending = true;
    }
}

void WirePollMain() {
    if (WireCanProgress()) BatteryFinish();
}
// Kept as a compatibility entry point. The main loop services every monitored
// stage before starter progress, rather than doing a second raw theft poll.
void WireTheftMonitorLoop() { WireServiceLoop(); }
