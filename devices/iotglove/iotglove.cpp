#include "iotglove.h"

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <esp_system.h>
#include <WiFi.h>

#include "battery.h"
#include "sensor.h"
#include "feedback.h"
#include "state_policy.h"
#include "library_and_pin.h"
#include "wifi_client.h"
#include "peer_state.h"
#include "ota_request.h"
#include "link_diagnostics.h"
#include "telnet.h"
#include "telnet_policy.h"

namespace {
using namespace iotglove;

constexpr bool kTraining = IOTGLOVE_TRAINING != 0;
GameModel game(kTraining ? Profile::Training : Profile::Origin);
FeedbackEngine feedbackEngine;
BatterySampler battery(IOTGLOVE_BATTERY_DIVIDER_RATIO, IOTGLOVE_BATTERY_CALIBRATION,
                       IOTGLOVE_BATTERY_MIN_MV, IOTGLOVE_BATTERY_MAX_MV);
HardwareSerial beetleSerial(1);
Adafruit_NeoPixel pixels(IOTGLOVE_LED_COUNT, IOTGLOVE_LED_PIN, NEO_GRB + NEO_KHZ800);
wire::Decoder decoder;
PeerState peer;
OtaRequests otaRequests;
LinkDiagnostics linkDiagnostics;
DiagnosticLog diagnosticLog;
BeetleLogTracker beetleLogs;
int firmware = 0, partition = 0;
uint32_t bootId = 0, requestSequence = 0, lastPing = 0, lastHeart = 0;
uint32_t inFlight = 0, lastSnapshotRequest = 0, lastLocationReport = 0;
uint32_t lastBatterySample = 0, lastBatteryReport = 0;
uint32_t resetStarted = 0, lastReset = 0;
bool networkReady = false, desiredScan = false, modeKnown = false;
bool resetLatched = false;
bool resetPending = false, resetHigh = false, resetTtgo = false, haveReset = false;
bool ttgoOtaSubmitted = false;
bool lastLocationValid = false, locationReported = false, haveOutputs = false;
char lastRoom[40] = {};
Outputs lastOutputs;
uint8_t lastBrightness = 0;
uint32_t rxBytes = 0, rxLines = 0, rxDecoded = 0, rxRejectedLines = 0;
uint32_t txBytes = 0, txFrames = 0, txFailures = 0;
uint32_t statusPrintedAt = 0, manualProbeAt = 0, transitionPrintedAt = 0;
uint32_t resetReportAt = 0, reportedBoot = 0, probeReportId = 0;
bool statusPrinted = false, manualProbeSent = false, transitionPrinted = false;
bool reportedKnown = false, reportedOnline = false, probeReportPending = false;
ResetObservation reportedReset = ResetObservation::Never;
bool serverReported = false, reportedServerFresh = false, reportedServerValid = false;
Phase reportedPhase = Phase::Unknown;
char reportedDevice[sizeof(ServerSnapshot::deviceName)] = {};

uint32_t nextId() {
  if (++requestSequence == 0) ++requestSequence;
  return requestSequence;
}

bool send(const wire::Frame& frame) {
  char line[wire::kMaxLine + 1];
  const size_t length = wire::format(line, sizeof(line), frame);
  if (!length) { ++txFailures; return false; }
  const size_t written = beetleSerial.write(reinterpret_cast<const uint8_t*>(line), length);
  txBytes += static_cast<uint32_t>(written);
  if (written != length) { ++txFailures; return false; }
  ++txFrames;
  return true;
}

void sendMode() {
  wire::Frame frame;
  strcpy(frame.type, "MODE"); frame.id = nextId();
  wire::put(frame, desiredScan ? "1" : "0");
  wire::put(frame, kTraining ? "training" : "live");
  send(frame);
  modeKnown = true;
}

void hello(uint32_t id) {
  wire::Frame frame;
  strcpy(frame.type, "HELLO"); frame.id = id;
  wire::put(frame, "ttgo");
  char value[16];
  snprintf(value, sizeof(value), "%d", firmware); wire::put(frame, value);
  snprintf(value, sizeof(value), "%d", partition); wire::put(frame, value);
  snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(bootId)); wire::put(frame, value);
  send(frame);
}

bool serverFresh(uint32_t now) {
  return game.server().valid && uint32_t(now - game.server().receivedAtMs) < kServerFreshMs;
}

bool updatePhase() {
  return otaAllowedPhase(game.server().phase);
}

bool otaBusy() {
  const auto status = networkOtaStatus();
  return otaRequests.hasActive() || peer.busy(millis()) ||
      status == NetworkOtaStatus::Queued || status == NetworkOtaStatus::Running;
}

const char* probeName(ProbeState state) {
  switch (state) {
    case ProbeState::Waiting: return "waiting";
    case ProbeState::Matched: return "matched";
    case ProbeState::Timeout: return "timeout";
    case ProbeState::SendFailed: return "send_failed";
    default: return "never";
  }
}

const char* resetName(ResetObservation state) {
  switch (state) {
    case ResetObservation::Waiting: return "waiting";
    case ResetObservation::RebootObserved: return "reboot_observed";
    case ResetObservation::BaselineUnknown: return "baseline_unknown_not_confirmed";
    case ResetObservation::Timeout: return "timeout_not_confirmed";
    default: return "never";
  }
}

void ageText(char* out, size_t size, bool known, uint32_t age) {
  if (known) snprintf(out, size, "%lu", static_cast<unsigned long>(age));
  else snprintf(out, size, "unknown");
}

const char* phaseName(Phase phase) {
  switch (phase) {
    case Phase::Setting: return "setting";
    case Phase::Ready: return "ready";
    case Phase::Exploration: return "exploration";
    case Phase::Active: return "active";
    case Phase::Ended: return "ended";
    default: return "unknown";
  }
}

const char* roleName(Role role) {
  switch (role) {
    case Role::Neutral: return "neutral";
    case Role::Player: return "player";
    case Role::Tagger: return "tagger";
    case Role::Ghost: return "ghost";
    default: return "unknown";
  }
}

void reportServerChanges(uint32_t now) {
  const ServerSnapshot& server = game.server();
  const bool fresh = serverFresh(now);
  if (serverReported && fresh == reportedServerFresh && server.valid == reportedServerValid &&
      server.phase == reportedPhase && !strcmp(server.deviceName, reportedDevice)) return;
  remoteConsoleLogf("[server] valid=%u fresh=%u device=%s phase=%s\n",
      server.valid, fresh, server.deviceName, phaseName(server.phase));
  serverReported = true; reportedServerFresh = fresh; reportedServerValid = server.valid;
  reportedPhase = server.phase;
  memcpy(reportedDevice, server.deviceName, sizeof(reportedDevice));
}

void printDiagnostics(uint32_t now) {
  if (statusPrinted && uint32_t(now - statusPrintedAt) < 1000U) return;
  statusPrinted = true; statusPrintedAt = now;
  char seen[16], helloAge[16], heartAge[16], rtt[16], resetAge[16];
  ageText(seen, sizeof(seen), peer.known(), peer.lastSeenAge(now));
  ageText(helloAge, sizeof(helloAge), peer.known(), peer.helloAge(now));
  ageText(heartAge, sizeof(heartAge), peer.heartbeatKnown(), peer.heartbeatAge(now));
  ageText(rtt, sizeof(rtt), linkDiagnostics.probeState() == ProbeState::Matched, linkDiagnostics.probeRtt());
  ageText(resetAge, sizeof(resetAge), linkDiagnostics.resetState() != ResetObservation::Never,
          linkDiagnostics.resetAge(now));
  // Fan-out directly: the complete status need not fit the small transition queue.
  const RemoteConsoleStatus console = remoteConsoleStatus();
  remoteConsoleLogf("[network] ip=%s MAC=%s boot=%lu telnet_listening=%u connected=%u\n"
      "[server] valid=%u fresh=%u device=%s phase=%s age_ms=%lu\n"
      "[beetle] reset_reason_known=%u reset_reason=%lu (explicit LOG snapshot)\n",
      WiFi.localIP().toString().c_str(), WiFi.macAddress().c_str(), (unsigned long)bootId,
      console.listening, console.connected, game.server().valid, serverFresh(now),
      game.server().deviceName, phaseName(game.server().phase),
      (unsigned long)(now - game.server().receivedAtMs),
      beetleLogs.reasonKnown(peer.bootId()),
      (unsigned long)(beetleLogs.reasonKnown(peer.bootId()) ? beetleLogs.reason() : 0));
  remoteConsoleLogf("[remote] dropped_usb_bytes=%lu dropped_telnet_bytes=%lu rejected_commands=%lu\n",
      (unsigned long)console.usbDroppedBytes, (unsigned long)console.telnetDroppedBytes,
      (unsigned long)console.rejectedCommands);
  remoteConsoleLogf("[inputs] chip_gpio26=%d button_gpio27=%d chip_debounced=%u button_debounced=%u chip_model=%u\n",
      sensorChipRaw(), sensorButtonRaw(),
      sensorChipPresent(), sensorButtonDown(), game.chipPresent());
  const ServerSnapshot& server = game.server();
  remoteConsoleLogf("[game] role=%s synchronized=%u life_chip=%ld captures_allowed=%u count=%u server_count=%u sacrificed=%u open=%u device_state=%s\n",
      roleName(server.role), game.synchronized(), (long)server.lifeChip, server.capturesAllowed,
      game.count(), server.revivalCount, server.sacrificed, server.open, deviceStateName(server.deviceState));
  // feedback() consumes event haptics. Diagnostics only read the last render.
  remoteConsoleLogf("[outputs] cache_valid=%u red=%u green=%u blue=%u lit=%u motor=%u brightness8=%u\n",
      haveOutputs, lastOutputs.red, lastOutputs.green, lastOutputs.blue, lastOutputs.lit,
      lastOutputs.motor, lastBrightness);
  diagnosticLog.append(
      "[diag] TTGO fw=%d partition=%d; UART TX32->6 RX36<-5 reset12->1\n"
      "[peer] known=%u online=%u fw=%lu partition=%lu boot=%lu last_seen_ms=%s hello_age_ms=%s\n"
      "[heart] known=%u fresh=%u age_ms=%s uptime_ms=%lu scan=%u ota_busy=%u (last sample)\n"
      "[probe] latest=%s id=%lu age_ms=%lu rtt_ms=%s matched_fresh=%u sent=%lu matched=%lu timeouts=%lu\n"
      "[uart] rx_bytes=%lu lines=%lu decoded=%lu accepted=%lu rejected_lines=%lu tx_bytes=%lu frames=%lu failures=%lu\n"
      "[location] fresh=%u room=%s\n"
      "[reset] reset_pending=%u pulse_high=%u last_pulse_result=%s last_pulse_age_ms=%s baseline_known=%u baseline_boot=%lu observed_boot=%lu\n"
      "[console] s/?=status p=PING b=Beetle-reset u=OTA; dropped_logs=%lu\n",
      firmware, partition, peer.known(), peer.online(now), (unsigned long)peer.firmwareVersion(),
      (unsigned long)peer.partitionVersion(), (unsigned long)peer.bootId(), seen, helloAge,
      peer.heartbeatKnown(), peer.heartbeatFresh(now), heartAge, (unsigned long)peer.heartbeatUptime(),
      peer.heartbeatScanEnabled(), peer.heartbeatOtaBusy(), probeName(linkDiagnostics.probeState()),
      (unsigned long)linkDiagnostics.probeId(), (unsigned long)linkDiagnostics.probeAge(now), rtt,
      linkDiagnostics.matchedFresh(peer, now), (unsigned long)linkDiagnostics.probesSent(),
      (unsigned long)linkDiagnostics.probesMatched(), (unsigned long)linkDiagnostics.probeTimeouts(),
      (unsigned long)rxBytes, (unsigned long)rxLines, (unsigned long)rxDecoded,
      (unsigned long)peer.acceptedFrames(), (unsigned long)rxRejectedLines,
      (unsigned long)txBytes, (unsigned long)txFrames, (unsigned long)txFailures,
      peer.locationFresh(now), peer.locationFresh(now) ? peer.room(now) : "unknown",
      resetPending, resetHigh, resetName(linkDiagnostics.resetState()), resetAge,
      linkDiagnostics.resetBaselineKnown(),
      (unsigned long)linkDiagnostics.resetBaselineBoot(), (unsigned long)linkDiagnostics.resetObservedBoot(),
      (unsigned long)diagnosticLog.dropped());
}

void sendProbe(uint32_t now, bool manual) {
  if (manual) {
    if (manualProbeSent && uint32_t(now - manualProbeAt) < 250U) return;
    manualProbeSent = true; manualProbeAt = now;
  }
  if (linkDiagnostics.probeState() != ProbeState::Waiting) {
    const uint32_t id = nextId();
    if (!linkDiagnostics.beginProbe(id, now)) return;
    wire::Frame ping; strcpy(ping.type, "PING"); ping.id = id;
    if (!send(ping)) linkDiagnostics.probeSendFailed();
    lastPing = now;
  }
  if (manual) {
    probeReportPending = true; probeReportId = linkDiagnostics.probeId();
    diagnosticLog.append("[probe] request id=%lu state=%s\n", (unsigned long)probeReportId,
                         probeName(linkDiagnostics.probeState()));
  }
}

void reportDiagnosticChanges(uint32_t now) {
  const bool known = peer.known(), online = peer.online(now);
  if ((known != reportedKnown || online != reportedOnline || (known && peer.bootId() != reportedBoot)) &&
      (!transitionPrinted || uint32_t(now - transitionPrintedAt) >= 500U)) {
    const char* change = !reportedKnown ? "discovered" :
        (known && peer.bootId() != reportedBoot ? "new_boot" : (online ? "online" : "offline"));
    transitionPrinted = true; transitionPrintedAt = now;
    if (diagnosticLog.append("[peer] %s known=%u online=%u fw=%lu partition=%lu boot=%lu\n", change,
        known, online, (unsigned long)peer.firmwareVersion(), (unsigned long)peer.partitionVersion(),
        (unsigned long)peer.bootId())) {
      reportedKnown = known; reportedOnline = online; reportedBoot = peer.bootId();
    }
  }
  if (probeReportPending && probeReportId == linkDiagnostics.probeId() &&
      linkDiagnostics.probeState() != ProbeState::Waiting) {
    diagnosticLog.append("[probe] id=%lu result=%s rtt_ms=%lu fresh=%u\n",
        (unsigned long)probeReportId, probeName(linkDiagnostics.probeState()),
        (unsigned long)(linkDiagnostics.probeState() == ProbeState::Matched ? linkDiagnostics.probeRtt() : 0),
        linkDiagnostics.matchedFresh(peer, now));
    probeReportPending = false;
  }
  const ResetObservation result = linkDiagnostics.resetState();
  if (result != ResetObservation::Never && result != ResetObservation::Waiting && result != reportedReset &&
      uint32_t(now - resetReportAt) >= 500U) {
    resetReportAt = now;
    const char* evidence = result == ResetObservation::RebootObserved ? "new_boot+post_pulse_PING_HELLO" :
        (result == ResetObservation::BaselineUnknown ? "PING_HELLO_without_baseline" : "none");
    if (diagnosticLog.append("[reset] %s baseline_boot=%lu observed_boot=%lu evidence=%s; reset_reason_source=explicit_LOG_only\n",
        resetName(result), (unsigned long)linkDiagnostics.resetBaselineBoot(),
        (unsigned long)linkDiagnostics.resetObservedBoot(), evidence)) reportedReset = result;
  }
}

void flushDiagnostics() {
  if (!diagnosticLog.pending()) return;
  size_t count = diagnosticLog.pending();
  remoteConsoleWrite(reinterpret_cast<const uint8_t*>(diagnosticLog.data()), count);
  diagnosticLog.consumed(count);
}

void pollPeer(uint32_t now) {
  wire::Frame frame;
  // Bounded receive work: an incoming flood cannot starve sensors or feedback.
  for (unsigned n = 0; n < 128 && beetleSerial.available(); ++n) {
    const char byte = static_cast<char>(beetleSerial.read());
    ++rxBytes;
    const bool decoded = decoder.feed(byte, now, frame);
    if (byte == '\n') { ++rxLines; if (!decoded) ++rxRejectedLines; }
    if (!decoded) continue;
    ++rxDecoded;
    diagnostics::Log log;
    if (diagnostics::parseLog(frame, log) && beetleLogs.accept(log, peer.known(), peer.bootId()))
      remoteConsoleLogf("[beetle] boot=%lu uptime_ms=%lu %s/%s value=%lu seq=%lu\n",
          (unsigned long)log.bootId, (unsigned long)log.uptimeMs, diagnostics::eventName(log.event),
          diagnostics::codeName(log.code), (unsigned long)log.value, (unsigned long)log.sequence);
    if (peer.receive(frame, now)) {
      linkDiagnostics.observeHello(frame.id, peer, now);
      modeKnown = false; sendMode();
    }
    if (strcmp(frame.type, "PING") == 0 && frame.count == 0) hello(frame.id);
  }
  peer.tick(now);
  linkDiagnostics.tick(now);
  reportDiagnosticChanges(now);
  if (uint32_t(now - lastPing) >= 2000U) {
    sendProbe(now, false);
  }
  if (uint32_t(now - lastHeart) >= 1000U) {
    lastHeart = now;
    wire::Frame heart; strcpy(heart.type, "HEART"); heart.id = nextId();
    char value[16]; snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(now));
    wire::put(heart, value); wire::put(heart, desiredScan ? "1" : "0");
    wire::put(heart, otaBusy() ? "1" : "0"); send(heart);
  }
}

void pollNetwork(uint32_t now) {
  if (kTraining || !networkReady) return;
  GameResult result;
  while (networkPollResult(result)) {
    if (result.sequence != inFlight) continue;
    inFlight = 0;
    if (result.status != GameResult::Status::Success) {
      game.commandUncertain(result.sequence);
      networkRequestSnapshot();
      remoteConsoleLogf("[server] Command not confirmed; waiting for authoritative state\n");
    }
  }
  ServerSnapshot snapshot;
  while (networkPoll(snapshot)) {
    game.applyServer(snapshot, now);
    if (otaRequests.observe(snapshot)) remoteConsoleLogf("[OTA] Malformed version command rejected\n");
    if (snapshot.resetRequested && !resetLatched) { resetPending = true; resetTtgo = true; }
    resetLatched = snapshot.resetRequested;
  }
  if (!serverFresh(now) && game.synchronized()) game.commandUncertain(0);
  if ((!game.synchronized() || !serverFresh(now)) && uint32_t(now - lastSnapshotRequest) >= 2000U) {
    lastSnapshotRequest = now;
    networkRequestSnapshot();
  }
  GameEvent event;
  if (!inFlight && !otaRequests.hasActive() && serverFresh(now) && game.peekEvent(event) && networkSubmit(event)) {
    inFlight = event.sequence;
    game.consumeEvent();
  }
}

void controlOta(uint32_t now) {
  if (kTraining || !networkReady) return;
  // Latch complete command identity. Changing a target while one pair runs can
  // queue the next pair, but cannot alter either target in the active pair.
  if (otaRequests.hasPending() && !otaRequests.hasActive() && !inFlight && serverFresh(now) && updatePhase() &&
      !resetHigh && !resetPending && peer.ota() != PeerOta::Waiting && !ttgoOtaSubmitted) {
    const uint32_t id = nextId();
    if (peer.beginOta(id, now, otaRequests.pending().beetleVersion)) {
      otaRequests.startPending(now);
      wire::Frame frame; strcpy(frame.type, "OTA"); frame.id = id;
      if (otaRequests.active().beetleVersion) {
        char target[16];
        snprintf(target, sizeof(target), "%lu", static_cast<unsigned long>(otaRequests.active().beetleVersion));
        wire::put(frame, "version"); wire::put(frame, target);
      } else wire::put(frame, "check");
      send(frame);
      remoteConsoleLogf("[OTA] Checking Beetle before TTGO\n");
    }
  }
  if (otaRequests.abortReadyPair(now, peer.ota() == PeerOta::Ready,
      ttgoOtaSubmitted, serverFresh(now), game.server().phase)) {
    peer.clearOta();
    remoteConsoleLogf("[OTA] TTGO stage cancelled: phase changed or pair deadline elapsed\n");
  }
  if (otaRequests.hasActive() && peer.ota() == PeerOta::Ready && !ttgoOtaSubmitted && !inFlight &&
      serverFresh(now) && updatePhase()) {
    if (networkRequestOta(otaRequests.active().ttgoVersion, otaRequests.active().sourceCommand)) {
      ttgoOtaSubmitted = true;
      remoteConsoleLogf("[OTA] Beetle confirmed; checking TTGO\n");
    }
  }
  if (otaRequests.hasActive() && peer.ota() == PeerOta::Failed) {
    remoteConsoleLogf("[OTA] Beetle failed/timed out; TTGO update stopped\n");
    otaRequests.finishActive();
    peer.clearOta();
  }
  const auto status = networkOtaStatus();
  if (ttgoOtaSubmitted && (status == NetworkOtaStatus::Skipped || status == NetworkOtaStatus::Failed)) {
    remoteConsoleLogf("%s\n", status == NetworkOtaStatus::Skipped ? "[OTA] Both boards checked" : "[OTA] TTGO update failed");
    networkClearOtaStatus();
    ttgoOtaSubmitted = false;
    peer.clearOta();
    otaRequests.finishActive();
  }
}

void controlReset(uint32_t now) {
  if (resetHigh && uint32_t(now - resetStarted) >= kResetPulseMs) {
    digitalWrite(IOTGLOVE_BEETLE_RESET_PIN, LOW);
    resetHigh = false;
    if (resetTtgo) { remoteConsoleLogf("[reset] Server watchdog: restarting TTGO\n"); ESP.restart(); }
  }
  if (resetPending && !resetHigh && !otaBusy() &&
      (!haveReset || uint32_t(now - lastReset) >= kResetCooldownMs)) {
    if (!resetTtgo) {
      linkDiagnostics.beginReset(peer, now);
      reportedReset = ResetObservation::Waiting;
      diagnosticLog.append("[reset] pulse HIGH %ums; fresh_HELLO_baseline=%u boot=%lu; observing up to %lums\n",
          kResetPulseMs, linkDiagnostics.resetBaselineKnown(),
          (unsigned long)linkDiagnostics.resetBaselineBoot(), (unsigned long)LinkDiagnostics::kResetTimeoutMs);
    }
    digitalWrite(IOTGLOVE_BEETLE_RESET_PIN, HIGH);
    resetStarted = lastReset = now;
    resetHigh = haveReset = true;
    resetPending = false;
    peer.invalidateLocation();
    if (resetTtgo) remoteConsoleLogf("[reset] Beetle WDT request pulse\n");
  }
}

void reportLocation(uint32_t now) {
  if (kTraining || !networkReady) return;
  const bool valid = peer.locationFresh(now) && desiredScan;
  const char* room = valid ? peer.room(now) : "";
  if (!locationReported || valid != lastLocationValid || strcmp(room, lastRoom) != 0 ||
      uint32_t(now - lastLocationReport) >= 2000U) {
    networkReportLocation(room);  // Empty clears stale location, never "unknown".
    strncpy(lastRoom, room, sizeof(lastRoom) - 1);
    lastLocationValid = valid;
    locationReported = true;
    lastLocationReport = now;
  }
}

void sampleBattery(uint32_t now) {
  if (!battery.configured() || uint32_t(now - lastBatterySample) < 20U) return;
  lastBatterySample = now;
  float voltage = 0;
  if (!battery.add(analogReadMilliVolts(IOTGLOVE_BATTERY_PIN), voltage)) return;
  if (lastBatteryReport != 0 && uint32_t(now - lastBatteryReport) < 60000U) return;
  lastBatteryReport = now;
  remoteConsoleLogf("[battery] %.2f V\n", voltage);
  if (!kTraining && networkReady) networkReportBattery(voltage);
}

void render(uint32_t now) {
  const Feedback state = game.feedback();
  const bool fresh = !kTraining && game.synchronized() && serverFresh(now) && peer.locationFresh(now);
  Outputs out = feedbackEngine.update(state, game.server().vibe, fresh, now, otaBusy() || resetHigh);
  if (otaBusy() || resetHigh) out.motor = false;
  digitalWrite(IOTGLOVE_MOTOR_PIN, out.motor ? HIGH : LOW);
  lastOutputs.motor = out.motor;  // Motor can change without any LED update.
  const uint8_t percent = kTraining ? 100 : game.server().brightness;
  const uint8_t brightness = static_cast<uint8_t>((uint16_t(percent) * 255U) / 100U);
  if (!haveOutputs || out.red != lastOutputs.red || out.green != lastOutputs.green ||
      out.blue != lastOutputs.blue || out.lit != lastOutputs.lit || brightness != lastBrightness) {
    pixels.setBrightness(brightness);
    for (uint8_t n = 0; n < IOTGLOVE_LED_COUNT; ++n)
      pixels.setPixelColor(n, n < out.lit ? pixels.Color(out.red, out.green, out.blue) : 0);
    pixels.show();
    lastOutputs = out;
    lastBrightness = brightness;
    haveOutputs = true;
  }
}
void handleConsoleCommand(char command, uint32_t now) {
    if (command == 'b') {
      if (!resetPending) {
        const char* waitFor = resetHigh ? "pulse_complete" : (otaBusy() ? "OTA" :
            (haveReset && uint32_t(now - lastReset) < kResetCooldownMs ? "cooldown" : "next_loop"));
        diagnosticLog.append("[reset] request accepted reset_pending=1 waiting_for=%s; last_pulse_result=%s belongs to the previous pulse\n",
            waitFor, resetName(linkDiagnostics.resetState()));
      }
      resetPending = true;
      if (!resetHigh) resetTtgo = false;
    }
    if (command == 'u' && !kTraining) otaRequests.requestLatest();
    if (command == 's' || command == '?') printDiagnostics(now);
    if (command == 'p') sendProbe(now, true);
}
}  // namespace

void gloveBegin(int firmwareVersion, int partitionVersion) {
  firmware = firmwareVersion; partition = partitionVersion;
  // Set output latch before enabling the driver: motor OFF and reset idle LOW.
  digitalWrite(IOTGLOVE_MOTOR_PIN, LOW); pinMode(IOTGLOVE_MOTOR_PIN, OUTPUT);
  digitalWrite(IOTGLOVE_BEETLE_RESET_PIN, LOW); pinMode(IOTGLOVE_BEETLE_RESET_PIN, OUTPUT);
  sensorConfigurePins();
  Serial.begin(115200);
  beetleSerial.begin(IOTGLOVE_UART_BAUD, SERIAL_8N1, IOTGLOVE_UART_RX, IOTGLOVE_UART_TX);
  pixels.begin(); pixels.clear(); pixels.show();
  const uint32_t now = millis();
  sensorBegin(now);
  game.begin(sensorChipPresent(), now);
  bootId = esp_random(); requestSequence = esp_random();
  if (!remoteConsoleBegin(firmware, partition, bootId, !kTraining))
    remoteConsoleLogf("[remote] initialization failed; USB diagnostics remain available\n");
  if (battery.configured()) {
    analogReadResolution(12);
    analogSetPinAttenuation(IOTGLOVE_BATTERY_PIN, ADC_11db);
  }
  remoteConsoleLogf("[glove] firmware=%d profile=%s\n", firmware, kTraining ? "training" : "origin");
  diagnosticLog.append("[diag] TTGO console ready: s/? status, p UART probe, b Beetle reset, u OTA\n");
  if (!battery.configured()) remoteConsoleLogf("[battery] Disabled: configure measured ADC divider and battery range\n");
  if (!kTraining) {
    networkReady = networkBegin(firmware, partition);
    if (networkReady) networkReportChip(sensorChipPresent());
  }
  hello(nextId()); sendMode();
}

void gloveLoop() {
  const uint32_t now = millis();
  pollPeer(now);
  pollNetwork(now);
  if (!kTraining) reportServerChanges(now);
  sensorPoll(game, now);
  if (!kTraining && networkReady) networkReportChip(sensorChipPresent());
  game.tick(now);
  // Read-only status and bounded PING probes work without a server connection.
  for (unsigned n = 0; n < 8 && Serial.available(); ++n) {
    const char command = static_cast<char>(Serial.read());
    handleConsoleCommand(command, now);
  }
  char remoteCommand;
  for (unsigned n = 0; n < 4 && remoteConsoleReadCommand(remoteCommand); ++n)
    handleConsoleCommand(remoteCommand, now);
  controlOta(now);
  controlReset(now);
  const bool scan = !kTraining && serverFresh(now) && game.server().phase == Phase::Active && !otaBusy();
  if (scan != desiredScan) { desiredScan = scan; modeKnown = false; if (!scan) peer.invalidateLocation(); }
  if (!modeKnown && peer.online(now)) sendMode();
  reportLocation(now);
  sampleBattery(now);
  render(now);
  flushDiagnostics();
  remoteConsolePoll();
  delay(1);  // Yield to Wi-Fi/RTOS; all timers above are nonblocking.
}
