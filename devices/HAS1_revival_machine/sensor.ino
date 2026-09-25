#include "HAS1_revival_machine.h"

//****************************************** Initialize ******************************************
void SensorInit()
{
  // Neopixel init
  pixels_top.begin();
  pixels_mid.begin();
  pixels_bot.begin();

  // 초기 전역 밝기 적용 (색상은 풀 밝기로 정의되어 있으므로 여기서 스케일)
  pixels_top.setBrightness(color_brightness);
  pixels_mid.setBrightness(color_brightness);
  pixels_bot.setBrightness(color_brightness);

  // mid는 device_state와 무관하게 항상 흰색 고정 — NeopixelSet()이 top/bot만 칠하므로
  // 여기서 한 번만 켜두면 이후 상태 전환에도 계속 흰색으로 유지된다.
  pixels_mid.fill(Adafruit_NeoPixel::Color(white[0], white[1], white[2]));
  pixels_mid.show();

  // Rfid init
  RfidInit();

  // Solenoid init
  SolenoidInit();
}

//********************************************* Rfid *********************************************
// Requested gain settings retained for field comparison; these values alone do
// not establish the cause of a distance-dependent failure. Configuration is only
// committed after a complete, validated PN532 response.
static GainMode currentGain = GAIN_NEAR;
static Pn532Deadline rfidOperationDeadline = {0, 0};
static bool rfidSweepActive = false;
static bool rfidPinsReady = false;
static const char *rfidHealthLogPending = nullptr;
static bool rfidUploadUidOnly = false;
static uint8_t rfidUploadUid[10];
static uint8_t rfidUploadUidLength = 0;

static uint8_t RfidGainCfg(int mode)
{
#if REVIVAL_RFID_DIAGNOSTICS
  if (mode == GAIN_DIAGNOSTIC_DEFAULT) return 0x59;
#endif
  return mode == GAIN_CONTACT ? 0x09 : mode == GAIN_NEAR ? 0x19 : 0x49;
}
#if REVIVAL_RFID_RUNTIME_TRACE
static uint8_t RfidRuntimeRequestedGainCfg() { return RfidGainCfg(currentGain); }
#endif

static void RfidClearMissingWindow()
{
  gameplay_tag_missing = false;
  gameplay_tag_miss_count = 0;
  gameplay_tag_missing_since_ms = 0;
}

static void RfidReportHealth(const char *state)
{
  rfidHealthLogPending = state; // Constants only; emitted after the scan/loop.
}

void RfidFlushHealthLog()
{
  if (!rfidHealthLogPending) return;
  char line[200];
  int length = snprintf(line, sizeof(line),
      "[RFID_HEALTH] state=%s phase=%s fault=%s status=0x%02X attempt=%u gain_known=%u\n",
      rfidHealthLogPending, pn532.phaseName(), pn532.faultName(), pn532.lastStatus(),
      rfid_recovery_attempts, rfid_gain_known ? 1 : 0);
  rfidHealthLogPending = nullptr;
  if (length > 0 && (size_t)length < sizeof(line))
    HardwareDebugSerial.write((const uint8_t *)line, (size_t)length);
}

static void RfidFault(Pn532Result result)
{
  rfid_last_outcome = result == Pn532Result::Deadline ? RfidReadOutcome::BudgetExceeded : RfidReadOutcome::TransportFault;
  bool newIncident = !rfid_recovery_required;
  rfid_recovery_required = true;
  rfid_gain_known = false;
  RfidClearMissingWindow();
  if (newIncident) {
    rfid_recovery_attempts = 0;
    rfid_recovery_locked = false;
    rfid_next_recovery_ms = (uint32_t)millis() + 1000;
    RfidReportHealth("fault");
  }
}

static bool ApplyGain(int mode)
{
  const uint32_t started = micros();
  Pn532Deadline deadline = rfidSweepActive ? rfidOperationDeadline : Pn532Deadline{(uint32_t)millis(), 100};
  Pn532Result result = pn532.setGain(RfidGainCfg(mode), deadline);
  bool ok = result == Pn532Result::Ok;
#if REVIVAL_RFID_DIAGNOSTICS
  int db = mode == GAIN_CONTACT ? 18 : mode == GAIN_NEAR ? 23 : mode == GAIN_DIAGNOSTIC_DEFAULT ? 38 : 33;
  RfidDiagnosticRecord(RFID_DIAG_GAIN, db, started, ok, nullptr, 0);
#elif REVIVAL_RFID_RUNTIME_TRACE
  RfidTraceRecord(RFID_TRACE_GAIN, RfidGainCfg(mode), started, ok, nullptr);
#else
  (void)started;
#endif
  if (!ok) { RfidFault(result); return false; }
  currentGain = (GainMode)mode;
  return true;
}

static bool ToggleRfField(bool on)
{
  const uint32_t started = micros();
  Pn532Deadline deadline = rfidSweepActive ? rfidOperationDeadline : Pn532Deadline{(uint32_t)millis(), 100};
  Pn532Result result = pn532.setRfField(on, deadline);
  bool ok = result == Pn532Result::Ok;
#if REVIVAL_RFID_DIAGNOSTICS
  RfidDiagnosticRecord(on ? RFID_DIAG_RF_ON : RFID_DIAG_RF_OFF,
                       RfidDiagnosticSoftwareGainDb(), started, ok, nullptr, 0);
#elif REVIVAL_RFID_RUNTIME_TRACE
  RfidTraceRecord(on ? RFID_TRACE_RF_ON : RFID_TRACE_RF_OFF,
                  RfidRuntimeRequestedGainCfg(), started, ok, nullptr);
#else
  (void)started;
#endif
  if (!ok) RfidFault(result);
  return ok;
}

static bool DetectAndRead(uint8_t outData[32])
{
  if (rfid_recovery_required || !rfid_gain_known) {
    rfid_last_outcome = RfidReadOutcome::Unavailable;
    RfidClearMissingWindow();
    return false;
  }
  Pn532Deadline deadline = rfidSweepActive ? rfidOperationDeadline : Pn532Deadline{(uint32_t)millis(), RFID_SCAN_BUDGET_MS};
  uint8_t uid[10];
  uint8_t uidLength = 0;
  uint32_t started = micros();
#if REVIVAL_RFID_DIAGNOSTICS
  Pn532Result result = pn532.readTarget(uid, uidLength, deadline.limited(rfid_diag_timeout_ms));
#else
  Pn532Result result = pn532.readTarget(uid, uidLength, deadline.limited(RFID_DETECT_TIMEOUT_MS));
#endif
  bool uidOk = result == Pn532Result::Ok;
#if REVIVAL_RFID_DIAGNOSTICS
  RfidDiagnosticRecord(RFID_DIAG_UID, RfidDiagnosticSoftwareGainDb(), started,
                       uidOk, uidOk ? uid : nullptr, uidOk ? uidLength : 0);
#elif REVIVAL_RFID_RUNTIME_TRACE
  RfidTraceRecord(RFID_TRACE_UID, RfidRuntimeRequestedGainCfg(), started, uidOk, nullptr);
#endif
  if (result == Pn532Result::NoTarget) { rfid_last_outcome = RfidReadOutcome::NoTarget; return false; }
  if (!uidOk) { RfidFault(result); return false; }
  if (rfidUploadUidOnly) {
    memcpy(rfidUploadUid, uid, uidLength);
    rfidUploadUidLength = uidLength;
    rfid_last_outcome = RfidReadOutcome::Read;
    return true;
  }
  started = micros();
  result = pn532.readPage7(outData, deadline);
  bool readOk = result == Pn532Result::Ok;
#if REVIVAL_RFID_DIAGNOSTICS
  RfidDiagnosticRecord(RFID_DIAG_READ, RfidDiagnosticSoftwareGainDb(), started,
                       readOk, readOk ? outData : nullptr, readOk ? 4 : 0);
#elif REVIVAL_RFID_RUNTIME_TRACE
  RfidTraceRecord(RFID_TRACE_PAGE7, RfidRuntimeRequestedGainCfg(), started, readOk, readOk ? outData : nullptr);
#else
  (void)started;
#endif
  if (readOk) rfid_last_outcome = RfidReadOutcome::Read;
  else if (result == Pn532Result::TagError) { rfid_last_outcome = RfidReadOutcome::TagReadFailed; RfidClearMissingWindow(); }
  else RfidFault(result);
  return readOk;
}

static bool DetectWithGainSwitch(uint8_t outData[32])
{
  rfidOperationDeadline = {(uint32_t)millis(), RFID_SCAN_BUDGET_MS};
  rfidSweepActive = true;
  const GainMode initialGain = currentGain;
  bool detected = DetectAndRead(outData);
  static const GainMode order[] = {GAIN_CONTACT, GAIN_NEAR, GAIN_FAR};
  for (uint8_t i = 0; !detected && rfid_last_outcome == RfidReadOutcome::NoTarget && i < 3; ++i) {
    if (order[i] == initialGain) continue; // Each requested gain is attempted once.
    if (!ApplyGain(order[i])) break;
    detected = DetectAndRead(outData);
  }
  // Re-selecting a stationary tag may require an RF field cycle. Only do so
  // after a fully validated no-target sweep, never after transport corruption.
  if (!detected && rfid_last_outcome == RfidReadOutcome::NoTarget) {
    // The three validated no-target responses remain valid absence evidence if
    // no room remains to start this optional cycle. Reserve only settle time;
    // command waits consume their remaining share of the same scan deadline.
    const Pn532Deadline sweepDeadline = rfidOperationDeadline;
    uint32_t remaining = sweepDeadline.remaining();
    if (remaining >= 20) {
      rfidOperationDeadline = sweepDeadline.limited(remaining - 12);
      bool offOk = ToggleRfField(false);
      rfidOperationDeadline = sweepDeadline;
      if (offOk) {
        delayMicroseconds(6000); // Full RF settling minimum, independent of RTOS tick phase.
        remaining = sweepDeadline.remaining();
        if (remaining <= 6) RfidFault(Pn532Result::Deadline);
        else {
          rfidOperationDeadline = sweepDeadline.limited(remaining - 6);
          bool onOk = ToggleRfField(true);
          rfidOperationDeadline = sweepDeadline;
          if (onOk) delayMicroseconds(6000);
        }
      }
    }
  }
  rfidSweepActive = false;
  return detected;
}

bool RfidInitializeHardware(bool recovery)
{
  rfid_recovery_required = true;
  rfid_gain_known = false;
  rfid_last_outcome = RfidReadOutcome::Unavailable;
  RfidClearMissingWindow();
  if (!rfidPinsReady) rfidPinsReady = pn532.beginPins();
  if (!rfidPinsReady) return false;
  const Pn532Deadline deadline = {(uint32_t)millis(), RFID_RECOVERY_BUDGET_MS};
  Pn532Result result = recovery ? pn532.abort(deadline) : Pn532Result::Ok;
  if (result == Pn532Result::Ok) result = pn532.wake(deadline);
  // SAM is the first command after the same-CS wake interval; no reset pin exists.
  if (result == Pn532Result::Ok) result = pn532.configureSam(deadline);
  const bool samConfigured = result == Pn532Result::Ok;
  (void)samConfigured;
  uint32_t version = 0;
  if (result == Pn532Result::Ok) result = pn532.getFirmwareVersion(version, deadline);
#if REVIVAL_RFID_DIAGNOSTICS
  rfid_diag_chip_firmware = version;
  rfid_diag_sam_ok = samConfigured;
  rfid_diag_retries_ok = false;
  rfid_diag_gain_ok = false;
  uint8_t retries = rfid_diag_retries;
#else
  const uint8_t retries = RFID_ACTIVATION_RETRIES;
#endif
  if (result == Pn532Result::Ok) result = pn532.setRetries(retries, deadline);
#if REVIVAL_RFID_DIAGNOSTICS
  rfid_diag_retries_ok = result == Pn532Result::Ok;
#endif
  if (result == Pn532Result::Ok) result = pn532.setGain(0x19, deadline);
#if REVIVAL_RFID_DIAGNOSTICS
  rfid_diag_gain_ok = result == Pn532Result::Ok;
#endif
  if (result == Pn532Result::Ok) {
    if (deadline.remaining() <= 6) result = Pn532Result::Deadline;
    else {
      result = pn532.setRfField(true, deadline.limited(deadline.remaining() - 6));
      if (result == Pn532Result::Ok) delayMicroseconds(6000);
    }
  }
  if (result != Pn532Result::Ok) { RfidFault(result); return false; }
  currentGain = GAIN_NEAR;
  rfid_gain_known = true;
  rfid_recovery_required = false;
  rfid_recovery_locked = false;
  return true;
}

bool RfidEnsureReady(bool allowRecovery)
{
  if (!rfid_recovery_required && rfid_gain_known) return true;
  rfid_last_outcome = RfidReadOutcome::Unavailable;
  RfidClearMissingWindow();
  if (!allowRecovery || revival_approval_pending || rfid_recovery_locked ||
      (int32_t)((uint32_t)millis() - rfid_next_recovery_ms) < 0) return false;
  ++rfid_recovery_attempts;
  if (RfidInitializeHardware(true)) { RfidReportHealth("recovered"); return false; } // Scan next loop, never a second 450ms budget here.
  if (rfid_recovery_attempts >= RFID_RECOVERY_MAX_ATTEMPTS) {
    rfid_recovery_locked = true;
    RfidReportHealth("locked");
  } else {
    const uint32_t backoff[] = {1000, 5000, 30000};
    rfid_next_recovery_ms = (uint32_t)millis() + backoff[rfid_recovery_attempts];
    RfidReportHealth("cooldown");
  }
  return false;
}

// Maintenance selects a card without requiring an existing G#P# payload.
// It shares the bounded gain sweep and recovery policy with gameplay.
Pn532Result RfidUploadSelect(uint8_t *uid, uint8_t &length)
{
  length = 0;
  if (!RfidEnsureReady(true)) return Pn532Result::Deadline;
  rfidUploadUidOnly = true;
  uint8_t unused[32];
  bool found = DetectWithGainSwitch(unused);
  rfidUploadUidOnly = false;
  if (found) {
    length = rfidUploadUidLength;
    memcpy(uid, rfidUploadUid, length);
    return Pn532Result::Ok;
  }
  if (rfid_last_outcome == RfidReadOutcome::NoTarget) return Pn532Result::NoTarget;
  if (rfid_last_outcome == RfidReadOutcome::TagReadFailed) return Pn532Result::TagError;
  if (rfid_last_outcome == RfidReadOutcome::TransportFault) return Pn532Result::TransportFault;
  return Pn532Result::Deadline;
}

void RfidUploadObserve(Pn532Result result)
{
  if (result == Pn532Result::TransportFault || result == Pn532Result::Deadline)
    RfidFault(result);
}

#if REVIVAL_RFID_DIAGNOSTICS
int RfidDiagnosticSoftwareGainDb()
{
  return currentGain == GAIN_CONTACT ? 18 : currentGain == GAIN_NEAR ? 23 :
         currentGain == GAIN_DIAGNOSTIC_DEFAULT ? 38 : 33;
}

bool RfidDiagnosticSetGainDb(int gainDb)
{
  if (rfid_recovery_required || !rfid_gain_known) return false;
  int mode;
  if (gainDb == 18) mode = GAIN_CONTACT;
  else if (gainDb == 23) mode = GAIN_NEAR;
  else if (gainDb == 33) mode = GAIN_FAR;
  else if (gainDb == 38) mode = GAIN_DIAGNOSTIC_DEFAULT;
  else return false;
  return ApplyGain(mode);
}

bool RfidDiagnosticRead(bool autoGain, uint8_t *data)
{
  return autoGain ? DetectWithGainSwitch(data) : DetectAndRead(data);
}

bool RfidDiagnosticSetRetries(uint8_t retries)
{
  if (rfid_recovery_required || !rfid_gain_known) return false;
  Pn532Result result = pn532.setRetries(retries, {(uint32_t)millis(), 100});
  if (result != Pn532Result::Ok) RfidFault(result);
  return result == Pn532Result::Ok;
}

bool RfidDiagnosticHardwareInit()
{
  rfid_recovery_attempts = 0;
  rfid_recovery_locked = false;
  // First diagnostic boot uses the same SAM-first startup as normal firmware.
  // Later explicit resets can abort an outstanding host transaction first.
  bool ok = RfidInitializeHardware(rfidPinsReady);
  if (!ok) { rfid_next_recovery_ms = (uint32_t)millis() + 1000; RfidReportHealth("diagnostic_fault"); }
  return ok;
}
#endif

/**
 * @brief RFID(=PN532) 세팅
 */
void RfidInit(void)
{
  if (RfidInitializeHardware(false)) {
    Serial.println("RFID 연결성공");
  } else {
    rfid_next_recovery_ms = (uint32_t)millis() + 1000;
    RfidReportHealth("startup_fault");
    Serial.println("!!!RFID 연결실패!!! - 승인/게임 루프는 계속 진행");
    // Existing startup-only notification; its HTTP time is outside the PN532
    // deadline. Runtime faults/recovery never send device or game state.
    has2wifi.Send((String)(const char *)my["device_name"], "device_state", "PN532");
  }
}

/**
 * @brief RFID 태그 인식
 */
void RfidLoop()
{
  if (revival_approval_pending)
  {
    AdminCardPollPending();
    return;
  }

  if (!rfid_tag)
  {
    rfid_tag = true;
    rfid_timer_id = rfid_timer.setTimeout(RFID_DEBOUNCE_MS, RfidTagTimerFunc);
  }
  else
  {
    return;
  }

  if (!RfidEnsureReady(true)) return;
  uint8_t data[32];
#if REVIVAL_RFID_RUNTIME_TRACE
  RfidTraceScanBegin("gameplay");
#endif
  bool detected = DetectWithGainSwitch(data);
#if REVIVAL_RFID_RUNTIME_TRACE
  RfidTraceScanEnd(detected, detected ? data : nullptr);
#endif
  ObserveGameplayTagOutcome(rfid_last_outcome);
  // tag_user_data 로그를 매번 남겨야 "읽히는데 무시되는 것"과 "아예 안 읽히는 것"을
  // 로그로 구분할 수 있다 - 둘 다 조용하면 디버깅이 안 된다.
  if (detected) CardChecking(data);
}

// 승인 조회를 먼저 처리하고, 조회하지 않는 루프에서만 1초 간격으로 관리자 카드를 확인한다.
// 느린 Gain 재설정/반대 Gain 재시도와 일반 게임 태그 처리는 승인 대기 경로에서 제외한다.
void AdminCardPollPending()
{
  if (revival_approval_polled_this_loop ||
      millis() - revival_approval_last_admin_poll_ms < REVIVAL_ADMIN_POLL_MS) return;

  if (!RfidEnsureReady(false)) return;
  uint8_t data[32];
#if REVIVAL_RFID_RUNTIME_TRACE
  RfidTraceScanBegin("admin_pending");
#endif
  bool detected = DetectAndRead(data);
#if REVIVAL_RFID_RUNTIME_TRACE
  RfidTraceScanEnd(detected, detected ? data : nullptr);
#endif
  revival_approval_last_admin_poll_ms = millis();
  if (!detected) return;
  String tagUser = "";
  for (int i = 0; i < 4; i++) tagUser += (char)data[i];
  if (tagUser == "MMMM") CardChecking(data);
}

/**
 * @brief ready 상태(activate_bool=false) 전용 경량 RFID 폴링.
 *        ready에서는 일반 게임 태그(G#P#)를 받지 않도록 RfidLoop() 자체가 꺼져 있으므로,
 *        MMMM 관리자 카드만은 상태 무관으로 열려야 한다는 요구를 이 함수가 별도로 담당한다.
 *        MMMM이 아니면 CardChecking()으로 넘기지 않고 조용히 무시 — ready의 기존 "태그 비활성"
 *        동작은 MMMM 외에는 그대로 유지된다. rfid_tag/rfid_timer는 RfidLoop()과 동일한 디바운스
 *        상태를 공유하지만, activate_bool로 loop()에서 상호 배타적으로만 호출되므로 안전하다.
 */
void AdminCardPollReady()
{
  if (!rfid_tag)
  {
    rfid_tag = true;
    rfid_timer_id = rfid_timer.setTimeout(RFID_DEBOUNCE_MS, RfidTagTimerFunc);
  }
  else
  {
    return;
  }

  if (!RfidEnsureReady(true)) return;
  uint8_t data[32];
#if REVIVAL_RFID_RUNTIME_TRACE
  RfidTraceScanBegin("admin_ready");
#endif
  bool detected = DetectWithGainSwitch(data);
#if REVIVAL_RFID_RUNTIME_TRACE
  RfidTraceScanEnd(detected, detected ? data : nullptr);
#endif
  if (!detected) return;

  String tagUser = "";
  for (int i = 0; i < 4; i++) tagUser += (char)data[i];
  if (tagUser != "MMMM") return;  // ready 상태에서는 MMMM 외 태그는 계속 무시

  CardChecking(data);
}

/**
 * @brief RFID에 태그된 NFC의 데이터에 따른 코드 동작
 *
 * @param rfidData 태그된 NFC의 데이터
 */
void CardChecking(uint8_t rfidData[32]) // 어떤 카드가 들어왔는지 확인용
{
  if (CardUploadBlocksGameplay()) return;
  String tagUser = "";
  for (int i = 0; i < 4; i++) // GxPx 데이터만 배열에서 추출해서 string으로 저장
    tagUser += (char)rfidData[i];
  Serial.println("tag_user_data : " + tagUser);

  // Outside maintenance, MMMM overrides ordinary game/device states.
  if (tagUser == "MMMM")
  {
    Serial.println("[RFID] admin card - opening (state-independent)");
    SolenoidPulse(SOLENOID_REVIVAL_PULSE_MS);
    return;
  }

  // 잘못 읽힌 카드가 임의의 서버 key로 전송되지 않도록 G#P# 형식을 검증한다.
  bool valid_tag_user =
      tagUser.length() == 4 &&
      tagUser[0] == 'G' && tagUser[1] >= '0' && tagUser[1] <= '9' &&
      tagUser[2] == 'P' && tagUser[3] >= '0' && tagUser[3] <= '9';
  if (!valid_tag_user)
  {
    Serial.println("[RFID] Invalid tag data (expected G#P#); request skipped");
    return;
  }

  // 역할 조회와 승인 왕복까지 포함해 실제 릴레이 반응시간을 잰다.
  const unsigned long tagDetectedMs = millis();

  String game_state_now = (String)(const char *)my["game_state"];

  if (game_state_now == "activate")
  {
    // 첫 요청의 사용자/계측값을 보존한다. 다른 태그도 승인 대기 중에는 끼어들지 않는다.
    if (revival_approval_pending) return;
    if (gameplay_tag_latched && gameplay_tag_user == tagUser) return;
    gameplay_tag_latched = true;
    gameplay_tag_user = tagUser;
    gameplay_tag_missing = false;
    gameplay_tag_miss_count = 0;
  }

  // tagger(사용 불가) 상태: 생존자든 유령이든 태그하면 역할 상관없이 보라색으로
  // 3번 점멸만 하고(사용 불가 알림) 열리거나 서버로 아무것도 보내지 않는다.
  if ((String)(const char *)my["device_state"] == "tagger")
  {
    Serial.println("[RFID] Tag while device_state=tagger - blink only, no action");
    NeoBlinkPurple(3);
    return;
  }

  // setting 상태: 역할 조회 없이, 유효한 형식(G#P#)의 태그면 누구든 태그할 때마다 연다.
  // 색상은 흰색 그대로 유지하고, device_state/game_state 모두 setting 그대로 둔다(서버 전송 없음).
  // 매번 다시 태그해도 또 열려야 하므로 중복 방지 래치를 걸지 않는다.
  if (game_state_now == "setting")
  {
    Serial.println("[RFID] Setting tag - opening");
    SolenoidPulse();
    return;
  }

  // activate 상태가 아니면(=ready/setting) 역할/서버 확인 없이 5초간 열기만 한다.
  // 게임 세팅을 위해 열어야 할 수도 있기 때문(device_state 무관하게 태그하면 열림).
  if (game_state_now != "activate")
  {
    Serial.println("[RFID] Tag outside gameplay (ready/setting) - opening 5s without role check");
    SolenoidPulse(SOLENOID_REVIVAL_PULSE_MS);
    return;
  }

  // 생명장치가 이미 열린 상태(서버 확정, device_state=="open")에서는 유령만 다시 열 수 있다.
  // 원래는 생존자가 태그해도 무조건 다시 열렸지만, open 이후 재입장은 유령 전용으로 제한한다.
  if ((String)(const char *)my["device_state"] == "open")
  {
    has2wifi.Receive(tagUser);
    String role = (String)(const char *)tag["role"];
    if (role == "ghost")
    {
      Serial.println("[RFID] Tag on already-open revival machine - ghost role, opening 5s");
      SolenoidPulse(SOLENOID_REVIVAL_PULSE_MS);
    }
    else
    {
      Serial.println("[RFID] Tag on already-open revival machine - non-ghost role(" + role + "), ignored");
    }
    return;
  }

  // is_open은 생명장치가 아니라 태그한 iotGlove 쪽 필드. 이미 true면(이 iotGlove가
  // 생명장치를 이미 연 적 있으면) 서버로 보내지 않고 사용 불가로 처리한다.
  // device_state=="tagger"와 달리 여기서는 점멸 후에도 device_state가 계속 "activate"라
  // NeoBlinkPurple만 쓰면 노란색(activate)으로 안 돌아오고 빨간색 점멸의 마지막 색에 머무르게 되므로 복원한다.
  const unsigned long roleReceiveStartMs = millis();
  has2wifi.Receive(tagUser);
  const unsigned long roleReceiveMs = millis() - roleReceiveStartMs;
  String tag_role = (String)(const char *)tag["role"];
  // roleReceiveMs는 role=="ghost"일 때만 [GhostTiming] 로그로 남았다 - 이 자리에서 매번
  // 찍어야 "태그 직후 지연이 Receive() HTTP 왕복 때문"인지 다른 role에서도 확인 가능하다.
  Serial.println("[RFID] " + tagUser + " is_open=" + String((int)tag["is_open"]) + " role=" + tag_role +
                 " (Receive took=" + String(roleReceiveMs) + "ms)");
  if ((int)tag["is_open"] != 0)
  {
    Serial.println("[RFID] iotGlove is_open=true - blink only, no action: " + tagUser);
    NeoBlinkPurple(3);
    NeopixelSet(yellow);  // activate 상태 색으로 복원
    return;
  }

  // 활성화 여부/역할(ghost·revival)/쿨다운/최초사용 여부는 모두 서버가 판단한다.
  // 이 기기는 태그 이벤트만 전달하고, 통과 시 device_state="open"이 폴링으로
  // 내려올 때(game_state.ino DataChange)에야 실제로 문을 연다.
  Serial.println("[RFID] Tag detected - sending situation to server: " + tagUser);
  last_open_tag_user = tagUser;  // DataChange()에서 open 확정 시 이 iotGlove의 is_open을 true로 쓰기 위해 기억

  // 유령 태그 -> 실제 릴레이 HIGH까지 측정. 사용자 조회 이전 시각을 사용하고,
  // game_state.ino에서 SolenoidPulse가 반환하는 HIGH 시각으로 종료한다.
  // role=="ghost"일 때만 기록한다 - 생존자/술래가 태그해도 서버가 open을 안 주는 게
  // 정상 동작이라, 이 경우까지 재면 매번 타임아웃으로 잡혀 로그가 오염된다.
  if (tag_role == "ghost")
  {
    ghost_tag_start_ms = tagDetectedMs;
    ghost_role_receive_ms = roleReceiveMs;
    ghost_poll_count = 0;
    ghost_rssi_at_tag = WiFi.RSSI();
    ghost_open_pending = true;
  }
  else
  {
    Serial.println("[GhostTiming] skip - role=" + tag_role + " (not ghost, open not expected)");
  }

  BeginRevivalApproval(tagDetectedMs);
  bool situation_sent;
  if (tag_role == "ghost")
  {
    // 유령 개방은 [GhostTiming] RELAY ON 로그의 situation= 값이 정확해야 하므로
    // 기존처럼 응답을 기다린다.
    unsigned long situationStartMs = millis();
    situation_sent = has2wifi.Situation(tagUser, "revival_machine");
    ghost_situation_ms = millis() - situationStartMs;
    Serial.println("[RFID] Situation send " + tagUser + " result=" + String(situation_sent ? "OK" : "FAIL") +
                   " took=" + String(ghost_situation_ms) + "ms");
  }
  else
  {
    // 비유령은 대부분 곧바로 거절로 끝나 Situation 결과를 몰라도 된다(아래에서 role
    // 기준으로 바로 승인 대기를 끝냄) - 응답을 기다리지 않고 비동기로 보내 그만큼의
    // 지연을 없앤다. 실제 전송 성공/시간은 비동기 태스크가 같은 형식으로 로그를 남긴다.
    has2wifi.SituationAsync(tagUser, "revival_machine");
    situation_sent = true;
  }

  // HTTP 200은 승인 자체가 아니다. 즉시 상태를 읽고, 미확정이면 전용 폴링으로 계속 확인한다.
  // API가 거부 사유를 노출하지 않으므로 상태가 그대로인 거부도 15초 상한으로 끝낸다.
  if (situation_sent)
  {
    PollRevivalApproval();
    // 서버 계약상 유령 외 역할은 개방 대상이 아니다. 이벤트/즉시 조회는 유지하되
    // 거부된 생존자 태그가 다음 유령의 사용을 15초 동안 막지 않도록 대기를 끝낸다.
    if (tag_role != "ghost")
    {
      EndRevivalApproval("role not eligible");
      // Receive()로 이미 확인한 role 기준으로(Situation 응답은 비동기라 기다리지 않음)
      // 유령이 아니라 거부됐다는 걸 태그한 사람에게 알려준다. device_state는 여기까지
      // 오면 "activate"이므로 NeoBlinkPurple이 빨간색으로 점멸한다.
      NeoBlinkPurple(3);
      NeopixelSet(yellow);  // activate 상태 색으로 복원
    }
  }
  else
    EndRevivalApproval("Situation failed", tag_role == "ghost");
}

bool RfidNsecTag(int sec)
{
  if (nsec_tag_num == 0 && !nsec_tag_bool)
  {
    nsec_tag_timer_id = nsec_tag_timer.setTimeout(5000, NsecTagTimerFailFunc);
    nsec_tag_bool = true;
  }
  else
  {
    nsec_tag_timer.restartTimer(nsec_tag_timer_id);
  }

  if (nsec_tag_num >= sec && nsec_tag_bool)
  {
    Serial.println("태그 성공");
    nsec_tag_timer.deleteTimer(nsec_tag_timer_id);
    nsec_tag_bool = false;
    nsec_tag_timer_id = nsec_tag_timer.setTimeout(2000, NsecTagTimerSuccessFunc);
    return true;
  }
  else
  {
    nsec_tag_num++;
  }
  return false;
}

// "사용 불가" 알림으로 짧게 점멸시킨다. device_state=="tagger"면 그 상태의 기본 색(보라색)으로,
// 그 외(예: activate 중 is_open 차단)는 빨간색으로 점멸한다. 점멸 후 상태 복원은 호출부 책임.
void NeoBlinkPurple(int times)
{
  int neoOff[3] = {0, 0, 0};
  int* blinkColor = ((String)(const char *)my["device_state"] == "tagger") ? purple : red;
  for (int i = 0; i < times; i++)
  {
    NeopixelSet(neoOff);
    delay(150);
    NeopixelSet(blinkColor);
    delay(150);
  }
}

//******************************************* Neopixel Helpers *******************************************
void NeopixelSet(int color[3])
{
  // mid는 건드리지 않는다 — device_state와 무관하게 항상 흰색 고정(SensorInit 참고).
  current_neopixel_color = color;
  uint32_t c = Adafruit_NeoPixel::Color(color[0], color[1], color[2]);
  pixels_top.fill(c); pixels_top.show();
  pixels_bot.fill(c); pixels_bot.show();
  delay(10);
  pixels_top.show();
  pixels_bot.show();
}

void ApplyBrightness(int raw)
{
  // raw: 0~255 전역 밝기. 색 배열은 풀 밝기(255)로 두고 setBrightness()로만 스케일.
  color_brightness = raw;
  pixels_top.setBrightness(raw);
  pixels_mid.setBrightness(raw);
  pixels_bot.setBrightness(raw);
  // 현재 켜져 있는 색을 새 밝기로 즉시 반영
  pixels_top.show();
  pixels_mid.show();
  pixels_bot.show();
}

void SetBrightness(int pct)
{
  // 값이 그대로면 건너뛴다 — 변경 감지를 호출부가 아니라 여기서 한다 (전 device 공통 방식).
  static int prevServerBrightness = -1;  // -1: 첫 호출은 반드시 적용
  if (pct == prevServerBrightness) return;
  prevServerBrightness = pct;

  int raw;
  if (pct <= 0 || pct > 100)
    raw = DEFAULT_BRIGHTNESS;
  else
    raw = map(pct, 1, 100, 1, 255);
  ApplyBrightness(raw);
}

void lightColor(Adafruit_NeoPixel &pixels, int color[3], int index)
{
  pixels.setPixelColor(index, color[0], color[1], color[2]);
  pixels.show();
}

//******************************************* Solenoid *******************************************
// 모스펫으로 구동되는 솔레노이드. HIGH = 통전(ON), LOW = 차단(OFF)로 가정.
// (배선이 반대라면 SolenoidOn/Off의 HIGH/LOW만 뒤집으면 됨)
void SolenoidInit()
{
  pinMode(SOLENOID_PIN, OUTPUT);
  digitalWrite(SOLENOID_PIN, LOW);
}

void SolenoidOn()
{
  digitalWrite(SOLENOID_PIN, HIGH);
}

void SolenoidOff()
{
  digitalWrite(SOLENOID_PIN, LOW);
}

// 평소엔 통전하지 않고, 잠금/해제가 바뀌는 순간에만 짧게 통전한다.
// (래치 없는 솔레노이드를 계속 통전 상태로 유지하면 장시간 발열/소손 위험이 있어 도입)
void SolenoidPulse()
{
  SolenoidPulse(SOLENOID_PULSE_MS);
}

unsigned long SolenoidPulse(unsigned long ms)
{
  SolenoidOn();
  const unsigned long relayOnMs = millis();
  delay(ms);
  SolenoidOff();
  return relayOnMs;
}

//******************************************* Neopixel *******************************************
void NeoNo()
{
}
// A 상태
void NeoBeforeTagger()
{
  delay(100);
  static int breathe = 0;
  static bool breathe_direction = true;

  breathe += breathe_direction ? BREATHE_STEP : -BREATHE_STEP;
  if (breathe >= BREATHE_MAX) { breathe = BREATHE_MAX; breathe_direction = false; }
  else if (breathe <= 0)      { breathe = 0;           breathe_direction = true;  }

  pixels_mid.fill(Adafruit_NeoPixel::Color(breathe, 0, 0)); pixels_mid.show();
  pixels_bot.fill(Adafruit_NeoPixel::Color(breathe, 0, 0)); pixels_bot.show();
  pixels_top.fill(Adafruit_NeoPixel::Color(red[0], red[1], red[2])); pixels_top.show();
}

void NeoTagger()
{
  delay(100);
  static int breathe_2 = 0;
  static bool breathe_direction_2 = true;

  breathe_2 += breathe_direction_2 ? BREATHE_STEP : -BREATHE_STEP;
  if (breathe_2 >= BREATHE_MAX) { breathe_2 = BREATHE_MAX; breathe_direction_2 = false; }
  else if (breathe_2 <= 0)      { breathe_2 = 0;           breathe_direction_2 = true;  }

  pixels_bot.fill(Adafruit_NeoPixel::Color(breathe_2, breathe_2, breathe_2)); pixels_bot.show();

  pixels_mid.clear();
  NeoArrow();
}

void NeoTaggerTag()
{
  static int tag_neo = 0;

  pixels_mid.clear();
  pixels_bot.clear();

  lightColor(pixels_mid, purple, tag_neo);

  if (++tag_neo > NUMPIXELS_MID)
  {
    tag_neo = 0;

    pixels_mid.clear();
    pixels_bot.clear();
    pixels_top.clear();
  }
}

void NeoAfterTagger()
{
  static bool after_tagger_neo_bool = false;

  if (after_tagger_neo_bool)
  {
    after_tagger_neo_bool = false;
    pixels_mid.clear();
    pixels_bot.clear();
    pixels_top.clear();
  }
  else
  {
    after_tagger_neo_bool = true;
    NeopixelSet(purple);
  }
}

void NeoGaming()
{
  delay(100);
  static int breathe = 0;
  static bool breathe_direction = true;

  breathe += breathe_direction ? BREATHE_STEP : -BREATHE_STEP;
  if (breathe >= BREATHE_MAX) { breathe = BREATHE_MAX; breathe_direction = false; }
  else if (breathe <= 0)      { breathe = 0;           breathe_direction = true;  }

  pixels_mid.fill(Adafruit_NeoPixel::Color(breathe, 0, breathe)); pixels_mid.show();
  pixels_bot.fill(Adafruit_NeoPixel::Color(breathe, 0, breathe)); pixels_bot.show();
  NeoArrow();
}

// void NeoTakenChip()
// {
//   static int chip_neo = 0;

//   if(chip_neo == 0){
//     pixels_bot.clear();
//     pixels_top.clear();
//     pixels_mid.clear();
//   }

//   pixels_mid.lightColor(purple, chip_neo);

//   if(++chip_neo > NUMPIXELS_MID){
//     chip_neo = 0;

//     pixels_mid.clear();
//     pixels_bot.clear();
//     pixels_top.clear();
//   }
// }

void NeoWin()
{
  static bool win_neo_bool = false;
  static int win_neo = 255;   // 풀 밝기 파랑, 실제 밝기는 setBrightness()가 결정
  static int win_neo_delay = 1500;

  win_neo_delay = win_neo_delay - 100;

  if (win_neo_bool)
  {
    win_neo_bool = false;
    int win_color[3] = {0, 0, win_neo};
    NeopixelSet(win_color);
  }
  else
  {
    win_neo_bool = true;
    pixels_mid.clear();
    pixels_bot.clear();
    pixels_top.clear();
  }

  if (win_neo_delay <= 300)
  {
    pixels_mid.clear();
    pixels_bot.clear();
    pixels_top.clear();

    NeoFunc = NeoNo;
  }
  delay(win_neo_delay);
}

void NeoLose()
{
  static bool lose_neo_bool = false;
  static int lose_neo = 255;   // 풀 밝기 빨강, 실제 밝기는 setBrightness()가 결정
  static int lose_neo_delay = 1500;

  lose_neo_delay = lose_neo_delay - 100;

  // 깜빡임을 표현
  if (lose_neo_bool)
  {
    lose_neo_bool = false;
    int lose_color[3] = {lose_neo, 0, 0};
    NeopixelSet(lose_color);
  }
  else
  {
    lose_neo_bool = true;
    pixels_mid.clear();
    pixels_bot.clear();
    pixels_top.clear();
  }

  if (lose_neo_delay <= 300)
  {
    pixels_mid.clear();
    pixels_bot.clear();
    pixels_top.clear();

    NeoFunc = NeoNo;
  }
  delay(lose_neo_delay);
}

void NeoArrow()
{
  static int arrow_pattern = 0;

  switch (arrow_pattern)
  {
  case 0:
    pixels_top.clear();
    break;

  case 1:
    arrow_neo_line_1 = 0;
    arrow_neo_line_2 = 16;
    arrow_neo_line_3 = 0;
    break;

  case 2:
    arrow_neo_line_1 = 1;
    arrow_neo_line_2 = 24;
    arrow_neo_line_3 = 1;
    break;

  case 3:
    arrow_neo_line_1 = 3;
    arrow_neo_line_2 = 12;
    arrow_neo_line_3 = 3;
    break;

  case 4:
    arrow_neo_line_1 = 6;
    arrow_neo_line_2 = 6;
    arrow_neo_line_3 = 6;
    break;

  case 5:
    arrow_neo_line_1 = 12;
    arrow_neo_line_2 = 3;
    arrow_neo_line_3 = 12;
    break;

  case 6:
    arrow_neo_line_1 = 24;
    arrow_neo_line_2 = 1;
    arrow_neo_line_3 = 24;
    break;

  case 7:
    arrow_neo_line_1 = 16;
    arrow_neo_line_2 = 0;
    arrow_neo_line_3 = 16;
    break;

  default:
    break;
  }

  if (++arrow_pattern > 7)
  {
    arrow_pattern = 0;
  }

  NeoArrowSet(1, arrow_neo_line_1);
  // NeoArrowSet(2, arrow_neo_line_2);
  NeoArrowSet(3, arrow_neo_line_3);
  pixels_top.show();
}

void NeoArrowSet(int arrow_neo_line_num, int arrow_neo_line)
{
  int neo_num = 0;

  if (arrow_neo_line_num == 1)
  {
    neo_num = 0;
  }
  else if (arrow_neo_line_num == 2)
  {
    neo_num = 5;
  }
  else if (arrow_neo_line_num == 3)
  {
    neo_num = 10;
  }

  switch (arrow_neo_line)
  {
  case 0:
    pixels_top.setPixelColor(neo_num + 1, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 2, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 3, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 4, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 5, 0, 0, 0);
    break;
  case 1:
    pixels_top.setPixelColor(neo_num + 1, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 2, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 3, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 4, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 5, 255, 0, 255);
    break;
  case 3:
    pixels_top.setPixelColor(neo_num + 1, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 2, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 3, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 4, 255, 0, 255);
    pixels_top.setPixelColor(neo_num + 5, 255, 0, 255);
    break;
  case 6:
    pixels_top.setPixelColor(neo_num + 1, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 2, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 3, 255, 0, 255);
    pixels_top.setPixelColor(neo_num + 4, 255, 0, 255);
    pixels_top.setPixelColor(neo_num + 5, 0, 0, 0);
    break;
  case 12:
    pixels_top.setPixelColor(neo_num + 1, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 2, 255, 0, 255);
    pixels_top.setPixelColor(neo_num + 3, 255, 0, 255);
    pixels_top.setPixelColor(neo_num + 4, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 5, 0, 0, 0);
    break;
  case 24:
    pixels_top.setPixelColor(neo_num + 1, 255, 0, 255);
    pixels_top.setPixelColor(neo_num + 2, 255, 0, 255);
    pixels_top.setPixelColor(neo_num + 3, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 4, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 5, 0, 0, 0);
    break;
  case 16:
    pixels_top.setPixelColor(neo_num + 1, 255, 0, 255);
    pixels_top.setPixelColor(neo_num + 2, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 3, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 4, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 5, 0, 0, 0);
    break;
  default:
    break;
  }
}
