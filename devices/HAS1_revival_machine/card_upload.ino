#include "HAS1_revival_machine.h"
#include "card_upload.h"
#include "card_ndef.h"
#include <Preferences.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Only volatile jobs exist. NVS contains validated encoder settings, never a job,
// armed flag, UID, server mode, or write result.
static CardNdef::Settings uploadSettings;
static CardNdef::Image uploadImage;
static uint8_t uploadMemory[144];
static bool uploadConnected = false, uploadActive = false, uploadExitGate = false;
static bool uploadSafeStateSeen = false, uploadExitRemoved = false;
static char uploadLine[192];
static uint16_t uploadLineLength = 0;
static bool uploadLineInvalid = false, uploadAfterCr = false;
static uint8_t uploadTelnetState = 0; // data, IAC, option, subnegotiation, subnegotiation-IAC
static char uploadLastResult[100] = "idle";
static uint8_t uploadUid[10], uploadUidLength = 0;
static uint8_t uploadLockPage = 0;
static bool uploadNtagI2c = false, uploadSessionChecked = false;
static uint16_t uploadOffset = 0, uploadWriteSize = 0;
static uint32_t uploadStarted = 0, uploadNextPoll = 0, uploadAbsentSince = 0;
static uint8_t uploadAbsentCount = 0;
static bool uploadAttemptedWrite = false, uploadSelected = false;
enum UploadJob { UPLOAD_NONE, UPLOAD_READ, UPLOAD_WRITE };
enum UploadStage { UPLOAD_REMOVE, UPLOAD_PRESENT, UPLOAD_VERSION, UPLOAD_STATIC,
                   UPLOAD_DYNAMIC, UPLOAD_MEMORY, UPLOAD_INVALIDATE, UPLOAD_EMPTY,
                   UPLOAD_BODY, UPLOAD_COMMIT, UPLOAD_CODE, UPLOAD_VERIFY };
static UploadJob uploadJob = UPLOAD_NONE;
static UploadStage uploadStage = UPLOAD_REMOVE;

static void UploadSay(const char *format, ...)
{
  if (!uploadConnected) return;
  char line[256];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(line, sizeof(line), format, arguments);
  va_end(arguments);
  CardUploadOutput(line);
}

static bool UploadSettingsValid(const CardNdef::Settings &settings)
{
  const char *error = nullptr;
  return CardNdef::validateSettings(settings, error);
}

static void UploadLoadSettings()
{
  uploadSettings = CardNdef::Settings();
  Preferences preferences;
  if (!preferences.begin("revival-card", true)) return;
  uint8_t bytes[105] = {};
  bool read = preferences.getBytesLength("config") == sizeof(bytes) &&
              preferences.getBytes("config", bytes, sizeof(bytes)) == sizeof(bytes);
  preferences.end();
  if (!read || memcmp(bytes, "CU01", 4) || !memchr(bytes + 8, 0, 97)) return;
  CardNdef::Settings candidate;
  candidate.format = (CardNdef::Format)bytes[4];
  candidate.prefix = (CardNdef::Prefix)bytes[5];
  candidate.layout = (CardNdef::Layout)bytes[6];
  memcpy(candidate.pattern, bytes + 8, sizeof(candidate.pattern));
  if (UploadSettingsValid(candidate)) uploadSettings = candidate;
}

static void UploadSaveSettings()
{
  if (!UploadSettingsValid(uploadSettings)) { UploadSay("ERROR invalid settings"); return; }
  uint8_t bytes[105] = {};
  memcpy(bytes, "CU01", 4);
  bytes[4] = (uint8_t)uploadSettings.format;
  bytes[5] = (uint8_t)uploadSettings.prefix;
  bytes[6] = (uint8_t)uploadSettings.layout;
  memcpy(bytes + 8, uploadSettings.pattern, sizeof(uploadSettings.pattern));
  Preferences preferences;
  if (!preferences.begin("revival-card", false)) { UploadSay("ERROR settings storage unavailable"); return; }
  size_t stored = preferences.putBytes("config", bytes, sizeof(bytes));
  preferences.end();
  UploadSay(stored == sizeof(bytes) ? "SAVED settings only" : "ERROR settings save failed");
}

static void UploadFinish(const char *reason, bool success)
{
  const char *kind = success ? "OK" : uploadAttemptedWrite ? "UNKNOWN" : "ERROR";
  snprintf(uploadLastResult, sizeof(uploadLastResult), "%s %s", kind, reason);
  UploadSay("%s", uploadLastResult);
  uploadJob = UPLOAD_NONE;
  uploadAttemptedWrite = false;
  uploadSelected = false;
  uploadUidLength = 0;
  uploadAbsentCount = 0;
}

static void UploadCancel(const char *reason)
{
  if (uploadJob != UPLOAD_NONE) UploadFinish(reason, false);
}

static void UploadStatus()
{
  UploadSay("mode=%s blocked=%u job=%s last=%s", uploadActive ? "card-upload" : "inactive",
            CardUploadBlocksGameplay() ? 1 : 0,
            uploadJob == UPLOAD_WRITE ? "write" : uploadJob == UPLOAD_READ ? "read" : "none", uploadLastResult);
  const char *prefixNames[] = {"auto", "none", "http://www.", "https://www.", "http://", "https://"};
  UploadSay("format=%s prefix=%s layout=%s template=%s",
            uploadSettings.format == CardNdef::Format::Uri ? "uri" : "text", prefixNames[(unsigned)uploadSettings.prefix],
            uploadSettings.layout == CardNdef::Layout::Game ? "game" : "standard", uploadSettings.pattern);
}

static void UploadHex(const char *label, const uint8_t *data, uint8_t length)
{
  static const char hex[] = "0123456789ABCDEF";
  char output[33];
  if (length > 16) length = 16;
  for (uint8_t i = 0; i < length; ++i) { output[2*i] = hex[data[i] >> 4]; output[2*i+1] = hex[data[i] & 15]; }
  output[2*length] = 0;
  UploadSay("%s%s", label, output);
}

static void UploadArm(bool writing, const char *value)
{
  if (writing) {
    const char *error = nullptr;
    if (!CardNdef::encode(uploadSettings, value, uploadImage, error)) {
      UploadSay("ERROR %s", error ? error : "invalid content"); return;
    }
    uploadWriteSize = (uint16_t)((uploadImage.size + 3) & ~3U);
    if (uploadWriteSize < 16) uploadWriteSize = 16; // Always overwrite stale page7 game data.
    if (uploadWriteSize > sizeof(uploadImage.bytes)) { UploadSay("ERROR image too large"); return; }
  }
  uploadJob = writing ? UPLOAD_WRITE : UPLOAD_READ;
  uploadStage = UPLOAD_REMOVE;
  uploadStarted = (uint32_t)millis();
  uploadNextPoll = uploadStarted;
  uploadAbsentCount = 0;
  uploadSelected = false;
  uploadAttemptedWrite = false;
  uploadNtagI2c = uploadSessionChecked = false;
  uploadUidLength = 0;
  uploadOffset = 0;
  memset(uploadMemory, 0, sizeof(uploadMemory));
  UploadSay("ARMED %s for 30s: remove the existing tag, then present one tag and hold it still.", writing ? "write" : "read");
}

static void UploadCommand(char *line)
{
  while (*line == ' ' || *line == '\t') ++line;
  char *end = line + strlen(line);
  while (end > line && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
  if (!*line) return;
  char *argument = strchr(line, ' ');
  if (argument) { *argument++ = 0; while (*argument == ' ') ++argument; }
  else argument = end;
  bool hasArgument = *argument != 0;
  if (!strcmp(line, "help") && !hasArgument) {
    UploadSay("help | status | read | preview <value> | write <value> | cancel | save | defaults");
    UploadSay("format uri|text | prefix auto|none|http://www.|https://www.|http://|https:// | template <pattern> | layout game|standard");
    UploadSay("read/write/save require server card-upload mode; write is not atomic on removal or power loss.");
    return;
  }
  if (!strcmp(line, "status") && !hasArgument) { UploadStatus(); return; }
  if (!strcmp(line, "cancel") && !hasArgument) { UploadCancel("cancelled; no automatic retry"); return; }
  if (!strcmp(line, "preview") && hasArgument) {
    CardNdef::Image image;
    const char *error = nullptr;
    if (!CardNdef::encode(uploadSettings, argument, image, error)) UploadSay("ERROR %s", error ? error : "invalid content");
    else {
      UploadSay("PREVIEW bytes=%u code=%s prefix=0x%02X content=%s", image.size, image.code, image.prefixCode, image.content);
      UploadHex("page7=", image.bytes + 12, 4);
    }
    return;
  }
  if (uploadJob != UPLOAD_NONE) { UploadSay("ERROR job already armed; cancel it first"); return; }
  if ((!strcmp(line, "write") && hasArgument) || (!strcmp(line, "read") && !hasArgument)) {
    if (!uploadActive) { UploadSay("ERROR server device_state must be card-upload"); return; }
    UploadArm(!strcmp(line, "write"), argument); return;
  }
  if (!strcmp(line, "save") && !hasArgument) {
    if (!uploadActive) { UploadSay("ERROR save requires card-upload mode"); return; }
    UploadSaveSettings(); return;
  }
  CardNdef::Settings candidate = uploadSettings;
  if (!strcmp(line, "defaults") && !hasArgument) candidate = CardNdef::Settings();
  else if (!strcmp(line, "format") && hasArgument) {
    if (!strcmp(argument, "uri")) candidate.format = CardNdef::Format::Uri;
    else if (!strcmp(argument, "text")) candidate.format = CardNdef::Format::Text;
    else { UploadSay("ERROR format must be uri or text"); return; }
  } else if (!strcmp(line, "layout") && hasArgument) {
    if (!strcmp(argument, "game")) candidate.layout = CardNdef::Layout::Game;
    else if (!strcmp(argument, "standard")) candidate.layout = CardNdef::Layout::Standard;
    else { UploadSay("ERROR layout must be game or standard"); return; }
  } else if (!strcmp(line, "template") && hasArgument) {
    if (strlen(argument) >= sizeof(candidate.pattern)) { UploadSay("ERROR template too long"); return; }
    memset(candidate.pattern, 0, sizeof(candidate.pattern));
    memcpy(candidate.pattern, argument, strlen(argument));
  } else if (!strcmp(line, "prefix") && hasArgument) {
    const char *names[] = {"auto", "none", "http://www.", "https://www.", "http://", "https://"};
    int index = 0;
    for (; index < 6 && strcmp(argument, names[index]); ++index) {}
    if (index == 6) { UploadSay("ERROR unsupported URI prefix"); return; }
    candidate.prefix = (CardNdef::Prefix)index;
  } else { UploadSay("ERROR unknown command; use help"); return; }
  if (!UploadSettingsValid(candidate)) { UploadSay("ERROR invalid settings syntax"); return; }
  uploadSettings = candidate;
  UploadSay("SETTINGS updated; use save to persist");
}

void CardUploadInit()
{
  uploadConnected = uploadActive = uploadExitGate = false;
  uploadSafeStateSeen = uploadExitRemoved = false;
  uploadJob = UPLOAD_NONE;
  uploadLineLength = 0;
  uploadLineInvalid = uploadAfterCr = false;
  uploadTelnetState = 0;
  strcpy(uploadLastResult, "idle");
  UploadLoadSettings();
}

void CardUploadConnected()
{
  uploadConnected = true;
  UploadCancel("connection replaced; job disarmed");
  uploadLineLength = 0;
  uploadLineInvalid = uploadAfterCr = false;
  uploadTelnetState = 0;
  UploadLoadSettings();
  UploadSay("Card upload console. Use help; no write resumes automatically.");
  UploadStatus();
}

void CardUploadDisconnected()
{
  uploadConnected = false;
  UploadCancel("disconnected; inspect card before retry");
  uploadLineLength = 0;
  uploadLineInvalid = uploadAfterCr = false;
  uploadTelnetState = 0;
}

void CardUploadInput(uint8_t byte)
{
  if (!uploadConnected) return;
  if (uploadTelnetState == 1) {
    if (byte == 250) uploadTelnetState = 3;
    else if (byte >= 251 && byte <= 254) uploadTelnetState = 2;
    else { uploadTelnetState = 0; if (byte == 255) uploadLineInvalid = true; }
    return;
  }
  if (uploadTelnetState == 2) { uploadTelnetState = 0; return; }
  if (uploadTelnetState == 3) { if (byte == 255) uploadTelnetState = 4; return; }
  if (uploadTelnetState == 4) { uploadTelnetState = byte == 240 ? 0 : 3; return; }
  if (byte == 255) { uploadTelnetState = 1; return; }
  if (uploadAfterCr && (byte == '\n' || byte == 0)) { uploadAfterCr = false; return; }
  uploadAfterCr = false;
  if (byte == '\r' || byte == '\n') {
    uploadLine[uploadLineLength] = 0;
    if (uploadLineInvalid) UploadSay("ERROR discarded invalid or overlong command");
    else UploadCommand(uploadLine);
    uploadLineLength = 0;
    uploadLineInvalid = false;
    uploadAfterCr = byte == '\r';
    return;
  }
  if (byte == 8 || byte == 127) { if (uploadLineLength && !uploadLineInvalid) --uploadLineLength; return; }
  if ((byte < 32 && byte != '\t') || byte == 0) { uploadLineInvalid = true; return; }
  if (uploadLineLength + 1 >= sizeof(uploadLine)) { uploadLineInvalid = true; return; }
  uploadLine[uploadLineLength++] = (char)byte;
}

bool CardUploadSyncMode(const char *state)
{
  bool active = state && !strcmp(state, "card-upload");
  if (active != uploadActive) {
    UploadCancel("server mode changed; inspect card before retry");
    uploadActive = active;
    uploadAbsentCount = 0;
    uploadNextPoll = (uint32_t)millis();
    uploadExitRemoved = uploadSafeStateSeen = false;
    uploadExitGate = !active;
    if (active) UploadSay("MODE card-upload; gameplay and opening blocked");
    else UploadSay("MODE exited; remove tag and wait for a safe server state");
  }
  if (!uploadActive && uploadExitGate)
    uploadSafeStateSeen = state && (!strcmp(state, "activate") || !strcmp(state, "tagger") ||
                                   !strcmp(state, "ready") || !strcmp(state, "setting"));
  if (uploadExitGate && uploadExitRemoved && uploadSafeStateSeen) uploadExitGate = false;
  return uploadActive;
}

bool CardUploadBlocksGameplay() { return uploadActive || uploadExitGate; }
bool CardUploadBlocksOpen() { return uploadActive || uploadExitGate; }

static bool UploadObserveRemoval(Pn532Result result)
{
  if (result != Pn532Result::NoTarget) { uploadAbsentCount = 0; return false; }
  if (!uploadAbsentCount) { uploadAbsentSince = (uint32_t)millis(); uploadAbsentCount = 1; return false; }
  if (uploadAbsentCount < 2) ++uploadAbsentCount;
  return (uint32_t)(millis() - uploadAbsentSince) >= 400;
}

static bool UploadWritableTlv()
{
  uint16_t offset = 0;
  bool ndefSeen = false, factoryLockSeen = false;
  while (offset < sizeof(uploadMemory)) {
    uint8_t type = uploadMemory[offset++];
    if (type == 0) continue;
    if (type == 0xFE) return true;
    if (type == 1 && uploadLockPage == 40 && !ndefSeen && !factoryLockSeen &&
        offset + 4 <= sizeof(uploadMemory)) {
      const uint8_t factoryLock[] = {3, 0xA0, 0x0C, 0x34};
      if (memcmp(uploadMemory + offset, factoryLock, sizeof(factoryLock))) return false;
      offset += sizeof(factoryLock); factoryLockSeen = true; continue;
    }
    if (type != 3 || ndefSeen || offset >= sizeof(uploadMemory)) return false;
    ndefSeen = true;
    uint16_t length = uploadMemory[offset++];
    if (length == 255) {
      if (offset + 2 > sizeof(uploadMemory)) return false;
      length = (uint16_t)((uploadMemory[offset] << 8) | uploadMemory[offset + 1]); offset += 2;
    }
    if (length > sizeof(uploadMemory) - offset) return false;
    offset += length;
  }
  return true; // Blank memory or an exactly bounded NDEF; no other TLV accepted.
}

static bool UploadCheckResult(Pn532Result result)
{
  RfidUploadObserve(result);
  if (result == Pn532Result::Ok) return true;
  UploadSay("PN532 phase=%s fault=%s tag_status=0x%02X",
            pn532.phaseName(), pn532.faultName(), pn532.lastTagStatus());
  UploadSay("PN532 session_step=%s response_len=%u response_first=%02X%02X",
            pn532.lastSessionStep(), pn532.lastSessionResponseLength(),
            pn532.lastSessionResponseByte(0), pn532.lastSessionResponseByte(1));
  UploadFinish("card/transport failure; inspect card before retry", false);
  return false;
}

void CardUploadLoop()
{
  uint32_t now = (uint32_t)millis();
  if (uploadJob != UPLOAD_NONE && (uint32_t)(now - uploadStarted) >= 30000) {
    UploadFinish("job timed out; no automatic retry", false); return;
  }
  if ((int32_t)(now - uploadNextPoll) < 0) return;
  if (!uploadActive && uploadExitGate && !uploadExitRemoved) {
    uint8_t uid[10], length = 0;
    Pn532Result result = RfidUploadSelect(uid, length);
    // Select handles reader health, including deferred scans after recovery.
    uploadNextPoll = (uint32_t)millis() + 200;
    if (UploadObserveRemoval(result)) {
      uploadExitRemoved = true;
      if (uploadSafeStateSeen) uploadExitGate = false;
      UploadSay("Tag removal confirmed; %s", uploadExitGate ? "waiting for safe server state" : "gameplay released");
    }
    return;
  }
  if (!uploadActive || !uploadConnected || uploadJob == UPLOAD_NONE) return;
  uploadNextPoll = now + 20;
  if (uploadStage == UPLOAD_REMOVE || uploadStage == UPLOAD_PRESENT || !uploadSelected) {
    uint8_t uid[10] = {}, length = 0;
    Pn532Result result = RfidUploadSelect(uid, length);
    // Select handles reader health, including deferred scans after recovery.
    if (uploadStage == UPLOAD_REMOVE) {
      uploadNextPoll = (uint32_t)millis() + 200;
      if (result != Pn532Result::Ok && result != Pn532Result::NoTarget) { UploadFinish("reader unavailable during removal check", false); return; }
      if (UploadObserveRemoval(result)) { uploadStage = UPLOAD_PRESENT; UploadSay("Ready: present one tag"); }
      return;
    }
    if (uploadStage == UPLOAD_PRESENT && result == Pn532Result::NoTarget) { uploadNextPoll = (uint32_t)millis() + 100; return; }
    if (result != Pn532Result::Ok || (length != 4 && length != 7 && length != 10)) { UploadFinish("tag lost or reader unavailable; no retry", false); return; }
    if (uploadStage == UPLOAD_PRESENT) {
      memcpy(uploadUid, uid, length); uploadUidLength = length;
      uploadStage = UPLOAD_VERSION; UploadHex("UID=", uid, length);
    } else if (length != uploadUidLength || memcmp(uid, uploadUid, length)) {
      UploadFinish("different UID; no retry", false); return;
    }
    uploadSelected = true;
    return;
  }
  // Original NTAG I2C has live SRAM/pass-through settings in sector 3. Inspect
  // them on the selected UID before each user-memory operation. The transport
  // restores sector 0; keep that selection for the following operation.
  if (uploadNtagI2c && uploadStage >= UPLOAD_MEMORY && !uploadSessionChecked) {
    uint8_t session[8] = {};
    const Pn532Deadline sessionDeadline = {(uint32_t)millis(), 250};
    if (!UploadCheckResult(pn532.readNtagI2cSession(session, sessionDeadline))) return;
    if (session[7] || !(session[6] & 1)) {
      UploadFinish("invalid NTAG I2C live session registers", false); return;
    }
    if ((session[0] & 0x42) || (session[6] & 0x46) ||
        (uploadJob == UPLOAD_WRITE && !(session[0] & 1))) {
      UploadFinish("NTAG I2C mirrored, pass-through, busy or RF write disabled", false); return;
    }
    uploadSessionChecked = true;
    return;
  }
  uploadSessionChecked = false;
  uploadSelected = false; // Every next operation needs another same-UID selection.
  const Pn532Deadline deadline = {(uint32_t)millis(), 100};
  uint8_t bytes[16] = {};
  Pn532Result result;
  if (uploadStage == UPLOAD_VERSION) {
    result = pn532.getTagVersion(bytes, deadline);
    if (!UploadCheckResult(result)) return;
    UploadHex("VERSION=", bytes, 8);
    const uint8_t versionPrefix[] = {0, 4, 4, 2, 1, 0};
    const uint8_t ntagI2cVersion[] = {0, 4, 4, 5, 2, 1, 0x13, 3};
    uploadNtagI2c = !memcmp(bytes, ntagI2cVersion, sizeof(ntagI2cVersion));
    if (!uploadNtagI2c && (memcmp(bytes, versionPrefix, sizeof(versionPrefix)) || bytes[7] != 3 ||
        (bytes[6] != 0x0F && bytes[6] != 0x11 && bytes[6] != 0x13))) {
      UploadFinish("unsupported tag; use NXP NTAG213/215/216 or NT3H1101", false); return;
    }
    uploadLockPage = bytes[6] == 0x0F ? 40 : bytes[6] == 0x11 ? 130 : 226;
    UploadSay("MODEL %s", uploadNtagI2c ? "NT3H1101 (NTAG I2C 1K)" :
              uploadLockPage == 40 ? "NTAG213" : uploadLockPage == 130 ? "NTAG215" : "NTAG216");
    if (uploadJob == UPLOAD_WRITE && uploadLockPage == 40) {
      const uint8_t emptyPrefix[5] = {};
      const uint8_t factoryLock[] = {1, 3, 0xA0, 0x0C, 0x34};
      if (uploadImage.size < 5 || memcmp(uploadImage.bytes, emptyPrefix, sizeof(emptyPrefix))) {
        UploadFinish("image has no reserved NTAG213 lock metadata", false); return;
      }
      memcpy(uploadImage.bytes, factoryLock, sizeof(factoryLock));
    }
    uploadStage = UPLOAD_STATIC;
  } else if (uploadStage == UPLOAD_STATIC) {
    result = pn532.readPages(2, bytes, deadline);
    if (!UploadCheckResult(result)) return;
    if (bytes[4] != 0xE1 || bytes[5] != 0x10 || bytes[6] < 0x12 || (bytes[7] & 0xF0)) {
      UploadFinish("unsupported capability container", false); return;
    }
    if (uploadNtagI2c && bytes[6] != 0x6D) {
      UploadFinish("unsupported NT3H1101 capability container size", false); return;
    }
    if (uploadJob == UPLOAD_WRITE && (bytes[2] || bytes[3] || (bytes[7] & 0x0F))) {
      UploadFinish("static lock or non-writable capability container", false); return;
    }
    uploadStage = uploadJob == UPLOAD_WRITE ? UPLOAD_DYNAMIC : UPLOAD_MEMORY;
  } else if (uploadStage == UPLOAD_DYNAMIC) {
    if (uploadNtagI2c) {
      // E2 is the original I2C 1K dynamic-lock page. READ fills subsequent
      // invalid pages with zero; these are not NTAG21x AUTH0/ACCESS registers.
      result = pn532.readPages(0xE2, bytes, deadline);
      if (!UploadCheckResult(result)) return;
      if (bytes[0] || bytes[1] || bytes[2]) {
        UploadFinish("NTAG I2C dynamic lock refused", false); return;
      }
      uploadStage = UPLOAD_MEMORY;
      return;
    }
    // Last user page + dynamic locks + CFG0 + CFG1. Never reads PWD/PACK pages.
    result = pn532.readPages((uint8_t)(uploadLockPage - 1), bytes, deadline);
    if (!UploadCheckResult(result)) return;
    if (bytes[4] || bytes[5] || bytes[6] || bytes[11] != 0xFF || bytes[12] || (bytes[8] & 0xC0)) {
      UploadFinish("locked, protected or mirrored tag refused", false); return;
    }
    uploadStage = UPLOAD_MEMORY;
  } else if (uploadStage == UPLOAD_MEMORY) {
    result = pn532.readPages((uint8_t)(4 + uploadOffset / 4), bytes, deadline);
    if (!UploadCheckResult(result)) return;
    memcpy(uploadMemory + uploadOffset, bytes, 16);
    if (uploadJob == UPLOAD_READ) { char label[24]; snprintf(label, sizeof(label), "page%u=", 4 + uploadOffset / 4); UploadHex(label, bytes, 16); }
    uploadOffset += 16;
    if (uploadOffset == sizeof(uploadMemory)) {
      if (uploadJob == UPLOAD_READ) { UploadFinish("READ complete: 144 user-memory bytes", true); return; }
      if (!UploadWritableTlv()) { UploadFinish("custom or out-of-range TLV refused", false); return; }
      uploadStage = UPLOAD_INVALIDATE; uploadOffset = 4;
    }
  } else if (uploadStage == UPLOAD_VERIFY) {
    result = pn532.readPages((uint8_t)(4 + uploadOffset / 4), bytes, deadline);
    if (!UploadCheckResult(result)) return;
    uint16_t count = uploadWriteSize - uploadOffset;
    if (count > 16) count = 16;
    if (memcmp(bytes, uploadImage.bytes + uploadOffset, count)) { UploadFinish("verification mismatch; inspect card", false); return; }
    uploadOffset += count;
    if (uploadOffset == uploadWriteSize) UploadFinish("WRITE verified; remove tag before another job", true);
  } else {
    uint8_t page;
    const uint8_t *data;
    const uint8_t invalidCode[] = {0, 0, 0, 0};
    const uint8_t emptyNdef[] = {3, 0, 0xFE, 0};
    if (uploadStage == UPLOAD_INVALIDATE) { page = 7; data = invalidCode; }
    else if (uploadStage == UPLOAD_EMPTY) { page = 4; data = emptyNdef; }
    else if (uploadStage == UPLOAD_COMMIT) { page = 4; data = uploadImage.bytes; }
    else if (uploadStage == UPLOAD_CODE) { page = 7; data = uploadImage.bytes + 12; }
    else { page = (uint8_t)(4 + uploadOffset / 4); data = uploadImage.bytes + uploadOffset; }
    uploadAttemptedWrite = true; // A failed ACK does not establish that EEPROM was unchanged.
    result = pn532.writePage(page, data, deadline);
    if (!UploadCheckResult(result)) return;
    if (uploadStage == UPLOAD_INVALIDATE) uploadStage = UPLOAD_EMPTY;
    else if (uploadStage == UPLOAD_EMPTY) { uploadStage = UPLOAD_BODY; uploadOffset = 4; }
    else if (uploadStage == UPLOAD_COMMIT) uploadStage = UPLOAD_CODE;
    else if (uploadStage == UPLOAD_CODE) { uploadStage = UPLOAD_VERIFY; uploadOffset = 0; }
    else {
      uploadOffset += 4;
      if (uploadOffset == 12) uploadOffset += 4; // page7 is committed last.
      if (uploadOffset >= uploadWriteSize) uploadStage = UPLOAD_COMMIT;
    }
  }
}
