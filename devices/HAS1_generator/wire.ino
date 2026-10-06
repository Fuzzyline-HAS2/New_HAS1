// All four connectors share one observed-count filter in every active stage.
// 100 ms samples, 1 s stability; gaps over 250 ms restart qualification.
static const uint8_t WIRE_PINS[4] = { WIRE_PIN_1, WIRE_PIN_2, WIRE_PIN_3, WIRE_PIN_4 };
static GeneratorWireState wireState;
static bool wireEnabled = false;
static bool wireContextValid = false;
static bool wireAttempted = false;
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

void WireInit() {
    for (int i = 0; i < 4; ++i) pinMode(WIRE_PINS[i], INPUT_PULLUP);
}
int WireCountPlugged() {
    int count = 0;
    for (int i = 0; i < 4; ++i) if (digitalRead(WIRE_PINS[i]) == LOW) ++count;
    return count;
}
int WireDisplayCount() { return wireState.stable() < 0 ? 0 : wireState.stable(); }

void WireResetTracking() {
    wireState.reset();
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
    if ((!enabled && wireEnabled) || (enabled && !wireEnabled) || enteringCharge || newIdentity) WireResetTracking();
    wireEnabled = enabled;
    // State writes also consume CAS revisions. Refresh that context without
    // replaying completion audio or resetting a tagger/starter session.
    if (changedServerState) wireContextValid = false;
    if (wireContextValid && wireState.acknowledged() &&
        ((int)my["battery_pack"] != wireState.acknowledgedCount() ||
         (int)my["max_battery_pack"] != wireContext.maximum)) {
        wireState.invalidateAck(); wireContextValid = false;
    }
    snprintf(wireObservedGame,sizeof(wireObservedGame),"%s",game ? game : "");
    snprintf(wireObservedDevice,sizeof(wireObservedDevice),"%s",state ? state : "");
    snprintf(wireObservedName,sizeof(wireObservedName),"%s",name ? name : "");
}

void WireSampleInputs(bool force) {
    if (!wireEnabled) return;
    const uint32_t now = millis();
    if (!force && !wireState.due(now)) return;
    const int previous = wireState.stable();
    if (wireState.sample(now, WireCountPlugged())) {
        Serial.printf("[Wire] stable=%d previous=%d maximum=%d\n", wireState.stable(), previous, (int)my["max_battery_pack"]);
        if (ptrCurrentMode == WirePollMain) {
            BatteryPackSend();
            if (previous >= 0 && wireState.stable() > previous) Mp3PlayLargeFolder(1,7);
        }
    }
}

bool WireCanProgress() {
    return wireEnabled && wireContextValid && WireTextEquals(my["game_state"],"activate") &&
        wireContext.maximum == (int)my["max_battery_pack"] &&
        wireState.ready(millis(), (int)my["max_battery_pack"]);
}
bool WireReadyForCompletion() {
    WireSampleInputs(true); // Recheck GPIO after synchronous audio/HTTP/RFID work.
    return WireCanProgress();
}

static bool WireAdoptContext(const GeneratorWireSnapshot &snapshot) {
    const bool changedEpoch = wireContext.epoch[0] && strcmp(wireContext.epoch,snapshot.epoch) != 0;
    const bool stateMismatch = !WireTextEquals(snapshot.gameState,my["game_state"]) ||
        !WireTextEquals(snapshot.deviceState,my["device_state"]) ||
        snapshot.maximum != (int)my["max_battery_pack"];
    if (!changedEpoch && wireContext.epoch[0] && snapshot.revision < wireContext.revision) {
        wireState.invalidateAck(); wireContextValid = false;
        Serial.println("[WireSync] stale revision rejected");
        return false;
    }
    wireContext = snapshot;
    if (changedEpoch || stateMismatch) {
        const bool resetCycle = changedEpoch || !WireTextEquals(snapshot.gameState,my["game_state"]) ||
            (WireTextEquals(snapshot.deviceState,"activate") && !WireTextEquals(my["device_state"],"activate"));
        if (resetCycle) WireResetTracking();
        else { wireState.invalidateAck(); wireContextValid = false; }
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
    if (!wireEnabled || !wireState.qualified(millis()) ||
        (wireContextValid && wireState.synced() && !wireFenceNeeded)) return;
    if (wireAttempted && uint32_t(millis()-wireLastAttempt) < 2000) return;
    wireAttempted = true;
    char name[32];
    snprintf(name,sizeof(name),"%s",wireObservedName);
    GeneratorWireSnapshot response;
    if (!wireContextValid) {
        const GeneratorWireResult result = WireHttpRequest(false,name,0,"",0,response);
        wireLastAttempt = millis();
        WireSampleInputs(true);
        if (result != GeneratorWireResult::Ok || !WireAdoptContext(response)) {
            wireState.invalidateAck(); wireContextValid = false; return;
        }
        if (response.count == wireState.stable() && !wireFenceNeeded) {
            wireState.acknowledge(response.count,wireState.generation());
            my["battery_pack"] = response.count; SyncBatteryPackCur();
            Serial.printf("[WireSync] confirmed=%d revision=%lu (read)\n",response.count,(unsigned long)response.revision);
            return;
        }
    }
    if (!wireState.qualified(millis())) return;
    const int sending = wireState.stable();
    const uint32_t generation = wireState.generation();
    const uint32_t expectedRevision = wireContext.revision;
    char expectedEpoch[37]; memcpy(expectedEpoch,wireContext.epoch,sizeof(expectedEpoch));
    // A timeout may mean that a request is still pending at the server. Even a
    // matching later GET must be fenced by a no-op CAS write before trusting it.
    wireFenceNeeded = true;
    const GeneratorWireResult result = WireHttpRequest(true,name,sending,expectedEpoch,expectedRevision,response);
    wireLastAttempt = millis();
    WireSampleInputs(true);
    if (result != GeneratorWireResult::Ok || strcmp(response.epoch,expectedEpoch) != 0 ||
        response.count != sending || response.revision <= expectedRevision || !WireAdoptContext(response)) {
        wireState.invalidateAck(); wireContextValid = false;
        Serial.println("[WireSync] unconfirmed; retry current stable count after resync");
        return;
    }
    if (wireState.acknowledge(response.count,generation)) {
        wireFenceNeeded = false;
        my["battery_pack"] = response.count; SyncBatteryPackCur();
        Serial.printf("[WireSync] confirmed=%d revision=%lu (CAS)\n",response.count,(unsigned long)response.revision);
    }
}

void WirePollMain() {
    if (WireCanProgress()) BatteryFinish();
}
// Kept as a compatibility entry point. The main loop services every monitored
// stage before starter progress, rather than doing a second raw theft poll.
void WireTheftMonitorLoop() { WireServiceLoop(); }
