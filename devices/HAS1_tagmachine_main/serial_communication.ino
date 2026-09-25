namespace {

// New Beetles publish H every two seconds. These values tolerate several
// missed frames and also give a Beetle time to finish PN532 initialization.
const unsigned long BEETLE_PING_INTERVAL_MS = 3000;
const unsigned long BEETLE_HEARTBEAT_STALE_MS = 8000;
const unsigned long BEETLE_RECOVERY_GRACE_MS = 6000;
const unsigned long BEETLE_RECOVERY_COOLDOWN_MS = 20000;
const unsigned long BEETLE_STATUS_UNKNOWN_MS = 10000;
const unsigned long BEETLE_DIAGNOSTIC_INTERVAL_MS = 30000;
const size_t BEETLE_RX_BYTE_BUDGET = 192;

unsigned long lastBeetleDiagnosticsMs = 0;

static bool Elapsed(unsigned long now, unsigned long since, unsigned long interval) {
  return (unsigned long)(now - since) >= interval;
}

static String AgeText(bool seen, unsigned long timestamp, unsigned long now) {
  if (!seen) return "-";
  return String((unsigned long)(now - timestamp)) + "ms";
}

static bool BeetleInputEnabled(int idx) {
  if (ptrCurrentMode != WhichTagged) return false;
  if (idx == BEETLE_MAIN) return ptrRfidMain == CommnunicationMainBeetle;
  return ptrRfidSub == CommnunicationBeetle;
}

static void SendBeetleControl(int idx, char command) {
  BeetleLinkState &link = beetleLinks[idx];
  if (link.serial == nullptr) return;
  link.serial->println(command);
}

static void MarkTransportActivity(BeetleLinkState &link, unsigned long now) {
  link.transportReady = true;
  link.lastRxMs = now;
  if (link.heartbeatStale) {
    link.heartbeatStale = false;
    link.staleSinceMs = 0;
    Serial.println(String("[BEETLE][") + link.name + "] link activity restored");
  }
}

static void MarkRfidReady(BeetleLinkState &link, unsigned long now) {
  link.rfidReady = true;
  link.rfidError = false;
  link.recoveryPending = false;
  link.statusUnknownSinceMs = 0;
  link.lastReadyMs = now;
  link.readyCount++;
}

static void QueueTagFrame(BeetleLinkState &link, const char *frame) {
  memcpy(link.pendingTag, frame, 4);
  link.pendingTag[4] = '\0';
  link.pendingTagValid = true;
  link.pendingRelayPulse = false;
}

static void HandleBeetleFrame(int idx, const char *frame, size_t length) {
  BeetleLinkState &link = beetleLinks[idx];
  const unsigned long now = millis();

  // Preserve the legacy prefix classifier order. W/R remain transport control
  // frames even if trailing bytes are present, rather than becoming tag data.
  if (length > 0 && frame[0] == 'W') {
    MarkTransportActivity(link, now);
    link.helloSeen = true;
    link.lastHelloMs = now;
    link.helloCount++;
    // W says the Beetle MCU booted, not that PN532 initialization completed.
    link.rfidReady = false;
    link.statusUnknownSinceMs = now;
    SendBeetleControl(idx, 'W');
    Serial.println(String("[BEETLE][") + link.name + "] boot hello; W ack sent");
    return;
  }

  if (length > 0 && frame[0] == 'R') {
    MarkTransportActivity(link, now);
    if (length == 1) {
      link.heartbeatCapable = true;
      MarkRfidReady(link, now);
      Serial.println(String("[BEETLE][") + link.name + "] PN532 recovery complete");
    } else {
      Serial.println(String("[BEETLE][") + link.name + "] legacy R-prefix status");
    }
    return;
  }

  // Every M-prefixed frame (including MMMM) is a gated relay event, never a
  // tag. Queue it so an inactive reader cannot bypass the historical ptr/WiFi
  // dispatch rules and open the door during another reader's login flow.
  if (length > 0 && frame[0] == 'M') {
    MarkTransportActivity(link, now);
    link.pendingRelayPulse = true;
    link.pendingTagValid = false;
    return;
  }

  if (length == 1) {
    switch (frame[0]) {
      case 'H':
        MarkTransportActivity(link, now);
        link.lastHeartbeatMs = now;
        link.heartbeatCount++;
        if (!link.heartbeatCapable) {
          link.heartbeatCapable = true;
          link.statusUnknownSinceMs = now;
          Serial.println(String("[BEETLE][") + link.name + "] heartbeat protocol detected");
        }
        // Do not echo H. New Beetles already send periodic heartbeats and
        // answer explicit probes; echoing every H would create a ping-pong loop.
        return;

      case 'A': {
        const bool stateChanged = !link.rfidReady || link.rfidError;
        MarkTransportActivity(link, now);
        link.heartbeatCapable = true;
        MarkRfidReady(link, now);
        if (stateChanged)
          Serial.println(String("[BEETLE][") + link.name + "] PN532 ready");
        return;
      }

      case 'E': {
        const bool stateChanged = !link.rfidError;
        MarkTransportActivity(link, now);
        link.heartbeatCapable = true;
        link.rfidReady = false;
        link.rfidError = true;
        link.lastErrorMs = now;
        link.errorCount++;
        if (link.statusUnknownSinceMs == 0) link.statusUnknownSinceMs = now;
        if (stateChanged)
          Serial.println(String("[BEETLE][") + link.name + "] PN532 error reported");
        return;
      }

      default:
        link.invalidFrameCount++;
        return;
    }
  }

  if (length >= 4) {
    MarkTransportActivity(link, now);
    link.lastTagMs = now;
    link.tagCount++;
    // A successful tag read is also direct evidence that PN532 is ready. This
    // keeps legacy Beetles useful even though they do not publish A/H frames.
    link.rfidReady = true;
    link.rfidError = false;
    link.lastReadyMs = now;
    QueueTagFrame(link, frame);
    return;
  }

  link.invalidFrameCount++;
}

static void ReadBeetleFramesWithBudget(int idx, size_t byteBudget) {
  BeetleLinkState &link = beetleLinks[idx];
  if (link.serial == nullptr) return;

  size_t bytesRead = 0;
  while (link.serial->available() > 0 && bytesRead < byteBudget) {
    const int value = link.serial->read();
    if (value < 0) break;
    bytesRead++;
    const char incoming = (char)value;

    if (incoming == '\r') continue;
    if (incoming == '\n') {
      if (link.rxOverflow) {
        link.invalidFrameCount++;
      } else if (link.rxLength > 0) {
        link.rxLine[link.rxLength] = '\0';
        HandleBeetleFrame(idx, link.rxLine, link.rxLength);
      }
      link.rxLength = 0;
      link.rxOverflow = false;
      continue;
    }

    if (link.rxOverflow) continue;
    if (link.rxLength < sizeof(link.rxLine) - 1) {
      link.rxLine[link.rxLength++] = incoming;
    } else {
      // Drain only this malformed frame up to its newline. Valid frames that
      // follow it remain parseable instead of being unconditionally flushed.
      link.rxOverflow = true;
      link.rxLength = 0;
    }
  }
}

static void ReadBeetleFrames(int idx) {
  ReadBeetleFramesWithBudget(idx, BEETLE_RX_BYTE_BUDGET);
}

static void DispatchPendingInput(int idx, bool forceDispatch) {
  BeetleLinkState &link = beetleLinks[idx];
  if (!link.pendingTagValid && !link.pendingRelayPulse) return;

  // The high-frequency link service may parse an input before its historical
  // consumer is due to run. Keep the single pending slot until either the
  // active game channel or the two-second WiFi compatibility path dispatches
  // it. Mode-transition flushes explicitly discard obsolete pending input.
  if (!forceDispatch && !BeetleInputEnabled(idx)) {
    return;
  }

  if (link.pendingRelayPulse) {
    link.pendingRelayPulse = false;
    link.pendingTagValid = false;
    digitalWrite(RELAY_PIN, HIGH);
    delay(500);
    digitalWrite(RELAY_PIN, LOW);
    return;
  }

  const String tagUser(link.pendingTag);
  link.pendingTagValid = false;
  link.pendingRelayPulse = false;
  mainRfidTagged = idx == BEETLE_MAIN;
  Serial.println(String("[BEETLE][") + link.name + "] tag " + tagUser);
  CheckingPlayers(tagUser);
  SendBeetleTag(idx,
                idx == BEETLE_MAIN ? "main_beetle_player" : "sub_beetle_player",
                tagUser);

  if (SubSerialTimerStart) {
    SubSerialTimer.deleteTimer(subSerialTimerId);
    SubSerialTimerStart = false;
  }
  subSerialTimerId = SubSerialTimer.setInterval(1000, SubSerialTimerFunc);
  SubSerialTimerStart = true;
}

static void RequestBeetleRecovery(int idx, const char *reason) {
  BeetleLinkState &link = beetleLinks[idx];
  const unsigned long now = millis();
  if (!link.heartbeatCapable) return;  // Never reset a legacy Beetle for silence.
  if (link.lastRecoveryMs != 0 &&
      !Elapsed(now, link.lastRecoveryMs, BEETLE_RECOVERY_COOLDOWN_MS)) return;

  SendBeetleControl(idx, 'R');
  link.lastRecoveryMs = now;
  link.recoveryPending = true;
  link.recoveryCount++;
  Serial.println(String("[BEETLE][") + link.name + "] recovery requested: " + reason);
}

static void MaintainBeetleLink(int idx) {
  BeetleLinkState &link = beetleLinks[idx];
  const unsigned long now = millis();

  if (link.lastPingMs == 0 || Elapsed(now, link.lastPingMs, BEETLE_PING_INTERVAL_MS)) {
    // An unanswered W can wake a legacy Beetle still in its boot handshake;
    // H discovers a running new-protocol Beetle after a TTGO-only reset.
    if (!link.transportReady) SendBeetleControl(idx, 'W');
    SendBeetleControl(idx, 'H');
    link.lastPingMs = now;
    link.pingCount++;
  }

  if (!link.heartbeatCapable) return;

  const bool activityRecent = link.lastRxMs != 0 &&
                              !Elapsed(now, link.lastRxMs, BEETLE_HEARTBEAT_STALE_MS);
  if (!activityRecent) {
    if (!link.heartbeatStale) {
      link.heartbeatStale = true;
      link.transportReady = false;
      link.staleSinceMs = now;
      Serial.println(String("[BEETLE][") + link.name + "] heartbeat stale; probing");
      SendBeetleControl(idx, 'H');
    } else if (Elapsed(now, link.staleSinceMs, BEETLE_RECOVERY_GRACE_MS)) {
      // This is best-effort for a one-way UART failure or a Beetle stalled in
      // PN532 code. A completely dead MCU still requires its own watchdog.
      RequestBeetleRecovery(idx, "heartbeat timeout");
    }
    return;
  }

  if (link.rfidError &&
      link.statusUnknownSinceMs != 0 &&
      Elapsed(now, link.statusUnknownSinceMs, BEETLE_RECOVERY_GRACE_MS)) {
    RequestBeetleRecovery(idx, "PN532 error");
  } else if (!link.rfidReady &&
             link.statusUnknownSinceMs != 0 &&
             Elapsed(now, link.statusUnknownSinceMs, BEETLE_STATUS_UNKNOWN_MS)) {
    RequestBeetleRecovery(idx, "PN532 status timeout");
  }
}

static void DiscardQueuedTags(int idx) {
  // Mode transitions used to throw away every byte in the UART FIFO. Parse
  // queued frames instead so W/H/A/E/R remain effective, and discard only
  // pending tag/relay events that belong to the mode being exited. Snapshot
  // the current backlog
  // so all bytes already queued are consumed even when it exceeds the normal
  // 192-byte service budget. Newly arriving bytes cannot make this loop
  // unbounded. Complete control/status frames are handled above; an incomplete
  // transition-time frame is discarded to preserve the old raw-flush boundary.
  BeetleLinkState &link = beetleLinks[idx];
  if (link.serial != nullptr) {
    const int queuedBytes = link.serial->available();
    if (queuedBytes > 0)
      ReadBeetleFramesWithBudget(idx, (size_t)queuedBytes);
  }
  link.rxLength = 0;
  link.rxOverflow = false;
  if (link.pendingTagValid || link.pendingRelayPulse) {
    link.pendingTagValid = false;
    link.pendingRelayPulse = false;
    link.droppedInputCount++;
  }
}

}  // namespace

BeetleLinkState beetleLinks[BEETLE_COUNT] = {};

void BeginBeetleLinkManagement() {
  beetleLinks[BEETLE_SUB].serial = &toSubSerial;
  beetleLinks[BEETLE_SUB].name = "Sub";
  beetleLinks[BEETLE_MAIN].serial = &toMainSerial;
  beetleLinks[BEETLE_MAIN].name = "Main";

  const unsigned long now = millis();
  for (int idx = 0; idx < BEETLE_COUNT; idx++) {
    // W is intentionally sent before H: an old Beetle that was still in its
    // two-second boot delay will consume the legacy handshake first.
    SendBeetleControl(idx, 'W');
    SendBeetleControl(idx, 'H');
    beetleLinks[idx].lastPingMs = now;
    beetleLinks[idx].pingCount = 1;
  }
  lastBeetleDiagnosticsMs = now;
  Serial.println("[BEETLE] link manager started; W/H probes sent to Main and Sub");
}

void ServiceBeetleLinks() {
  // Drain both UARTs before any mode handler or HTTP work can block. This
  // function intentionally does not dispatch tags: it services transport and
  // control/status frames only, while retaining one pending tag per channel.
  ReadBeetleFrames(BEETLE_SUB);
  ReadBeetleFrames(BEETLE_MAIN);

  MaintainBeetleLink(BEETLE_SUB);
  MaintainBeetleLink(BEETLE_MAIN);

  const unsigned long now = millis();
  if (Elapsed(now, lastBeetleDiagnosticsMs, BEETLE_DIAGNOSTIC_INTERVAL_MS)) {
    lastBeetleDiagnosticsMs = now;
    PrintBeetleLinkDiagnostics();
  }
}

void DispatchBeetleTagsFromWifiTick() {
  // Preserve the legacy behavior: even in ready/setting, the two-second WiFi
  // tick accepts both readers (notably the MMMM staff card). Sub remains first,
  // matching the original WifiIntervalFunc ordering.
  ReadBeetleFrames(BEETLE_SUB);
  ReadBeetleFrames(BEETLE_MAIN);
  DispatchPendingInput(BEETLE_SUB, true);
  DispatchPendingInput(BEETLE_MAIN, true);
}

void PrintBeetleLinkDiagnostics() {
  const unsigned long now = millis();
  for (int idx = 0; idx < BEETLE_COUNT; idx++) {
    BeetleLinkState &link = beetleLinks[idx];
    String line = String("[BEETLE][") + link.name + "] transport=" +
                  (link.transportReady ? "ready" : "unknown") +
                  " protocol=" + (link.heartbeatCapable ? "heartbeat" : "legacy/unknown") +
                  " rfid=" + (link.rfidReady ? "ready" : (link.rfidError ? "error" : "unknown")) +
                  " age(rx/hello/hb/ready/tag)=" +
                  AgeText(link.lastRxMs != 0, link.lastRxMs, now) + "/" +
                  AgeText(link.helloSeen, link.lastHelloMs, now) + "/" +
                  AgeText(link.heartbeatCount != 0, link.lastHeartbeatMs, now) + "/" +
                  AgeText(link.readyCount != 0 || link.tagCount != 0, link.lastReadyMs, now) + "/" +
                  AgeText(link.tagCount != 0, link.lastTagMs, now) +
                  " count(W/H/A/E/tag/R/invalid/drop)=" +
                  String(link.helloCount) + "/" + String(link.heartbeatCount) + "/" +
                  String(link.readyCount) + "/" + String(link.errorCount) + "/" +
                  String(link.tagCount) + "/" + String(link.recoveryCount) + "/" +
                  String(link.invalidFrameCount) + "/" + String(link.droppedInputCount);
    Serial.println(line);
  }
}

void CommnunicationBeetle() {
  ReadBeetleFrames(BEETLE_SUB);
  DispatchPendingInput(BEETLE_SUB, false);
  MaintainBeetleLink(BEETLE_SUB);
}

void CommnunicationMainBeetle() {
  ReadBeetleFrames(BEETLE_MAIN);
  DispatchPendingInput(BEETLE_MAIN, false);
  MaintainBeetleLink(BEETLE_MAIN);
}

/**
 * @brief Beetle tag value server reporting (one-second throttle per channel).
 */
void SendBeetleTag(int idx, const char *field, const String &tagUser) {
  if (tagUser == "MMMM") return;
  const unsigned long now = millis();
  if (beetleSendLastMs[idx] != 0 && now - beetleSendLastMs[idx] < 1000) return;
  beetleSendLastMs[idx] = now;
  has2wifi.Send((String)(const char *)my["device_name"], field, tagUser);
}

void SubSerialFlush() {
  DiscardQueuedTags(BEETLE_SUB);
}

void MainSerialFlush() {
  DiscardQueuedTags(BEETLE_MAIN);
}
