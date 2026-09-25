#include "pn532_transport.h"
#include <string.h>

RevivalPn532::RevivalPn532(uint8_t sck, uint8_t miso, uint8_t mosi, uint8_t ss)
  : bus_(ss, sck, miso, mosi, 1000000, SPI_BITORDER_LSBFIRST, SPI_MODE0) {}

bool RevivalPn532::beginPins() { return bus_.begin(); }

Pn532Result RevivalPn532::fail(Pn532Fault fault, Pn532Result result)
{
  fault_ = fault;
  target_ = 0;
  return result;
}

Pn532Result RevivalPn532::wake(const Pn532Deadline &deadline)
{
  if (deadline.remaining() < 3) return fail(Pn532Fault::Budget, Pn532Result::Deadline);
  wakePending_ = true; // Hold CS low for 2 ms immediately before the next command bytes.
  return Pn532Result::Ok;
}

Pn532Result RevivalPn532::abort(const Pn532Deadline &deadline)
{
  phase_ = Pn532Phase::Write;
  if (deadline.remaining() < 3) return fail(Pn532Fault::Budget, Pn532Result::Deadline);
  const uint8_t packet[] = {0x01, 0, 0, 0xFF, 0, 0xFF, 0};
  // A diagnostic reset can be the first access after PN532 power-up. Keep CS
  // asserted through the wake interval and host ACK; no response is expected.
  bus_.beginTransactionWithAssertingCS();
  delayMicroseconds(2000);
  for (uint8_t i = 0; i < sizeof(packet); ++i) bus_.transfer(packet[i]);
  bus_.endTransactionWithDeassertingCS();
  target_ = 0;
  return deadline.expired() ? fail(Pn532Fault::Budget, Pn532Result::Deadline) : Pn532Result::Ok;
}

Pn532Result RevivalPn532::waitReady(const Pn532Deadline &deadline, bool ack)
{
  phase_ = ack ? Pn532Phase::AckWait : Pn532Phase::ResponseWait;
  while (!deadline.expired()) {
    const uint8_t statusCommand = 0x02;
    if (!bus_.write_then_read(&statusCommand, 1, &status_, 1)) return fail(Pn532Fault::BusIo);
    // RDY is bit0, but RCV_OVR/TR_FE are faults, not additional ready states.
    // Unexpected reserved bits also make the transport state untrustworthy.
    if (status_ & 0x0C) return fail(Pn532Fault::StatusFlags);
    if (status_ & 0xFE) return fail(Pn532Fault::InvalidStatus);
    if (status_ & 0x01) return Pn532Result::Ok;
    delay(1);
  }
  return fail(ack ? Pn532Fault::AckTimeout : Pn532Fault::ResponseTimeout, Pn532Result::Deadline);
}

Pn532Result RevivalPn532::readAck(const Pn532Deadline &deadline)
{
  phase_ = Pn532Phase::AckRead;
  if (deadline.expired()) return fail(Pn532Fault::Budget, Pn532Result::Deadline);
  const uint8_t readCommand = 0x03;
  uint8_t ack[6];
  if (!bus_.write_then_read(&readCommand, 1, ack, sizeof(ack))) return fail(Pn532Fault::BusIo);
  if (deadline.expired()) return fail(Pn532Fault::Budget, Pn532Result::Deadline);
  const uint8_t expected[] = {0, 0, 0xFF, 0, 0xFF, 0};
  const uint8_t nack[] = {0, 0, 0xFF, 0xFF, 0, 0};
  if (!memcmp(ack, nack, sizeof(ack))) return fail(Pn532Fault::Nack);
  return memcmp(ack, expected, sizeof(ack)) ? fail(Pn532Fault::BadAck) : Pn532Result::Ok;
}

Pn532Result RevivalPn532::readResponse(uint8_t expected, uint8_t *data, uint8_t &length,
                                     const Pn532Deadline &deadline)
{
  phase_ = Pn532Phase::ResponseRead;
  length = 0;
  uint8_t header[5], frame[66]; // At most LEN=64 bytes plus checksum and postamble.
  if (deadline.expired()) return fail(Pn532Fault::Budget, Pn532Result::Deadline);
  bus_.beginTransactionWithAssertingCS();
  bus_.transfer(0x03);
  for (uint8_t i = 0; i < sizeof(header); ++i) header[i] = bus_.transfer(0xFF);
  if (header[0] || header[1] || header[2] != 0xFF) {
    bus_.endTransactionWithDeassertingCS();
    return fail(Pn532Fault::BadFrame);
  }
  uint8_t count = header[3];
  if (!count || count > 64 || (uint8_t)(count + header[4]) != 0) {
    bus_.endTransactionWithDeassertingCS();
    return fail(Pn532Fault::FrameLength);
  }
  for (uint8_t i = 0; i < count + 2; ++i) {
    if (deadline.expired()) {
      bus_.endTransactionWithDeassertingCS();
      return fail(Pn532Fault::Budget, Pn532Result::Deadline);
    }
    frame[i] = bus_.transfer(0xFF);
  }
  bus_.endTransactionWithDeassertingCS();
  if (deadline.expired()) return fail(Pn532Fault::Budget, Pn532Result::Deadline);
  uint8_t checksum = 0;
  for (uint8_t i = 0; i <= count; ++i) checksum += frame[i];
  if (checksum || frame[count + 1]) return fail(Pn532Fault::Checksum);
  if (count == 1 && frame[0] == 0x7F) return fail(Pn532Fault::ErrorFrame);
  if (count < 2 || frame[0] != 0xD5 || frame[1] != expected)
    return fail(Pn532Fault::UnexpectedResponse);
  length = count - 2;
  if (length) memcpy(data, frame + 2, length);
  return Pn532Result::Ok;
}

Pn532Result RevivalPn532::command(const uint8_t *data, uint8_t length, uint8_t *response,
                                 uint8_t &responseLength, const Pn532Deadline &deadline)
{
  fault_ = Pn532Fault::None;
  phase_ = Pn532Phase::Write;
  responseLength = 0;
  if (!length || length > 20) return fail(Pn532Fault::FrameLength);
  if (deadline.expired()) return fail(Pn532Fault::Budget, Pn532Result::Deadline);
  uint8_t packet[29];
  packet[0] = 0x01;
  packet[1] = packet[2] = 0;
  packet[3] = 0xFF;
  packet[4] = length + 1;
  packet[5] = (uint8_t)(0 - packet[4]);
  packet[6] = 0xD4;
  uint8_t checksum = 0xD4;
  for (uint8_t i = 0; i < length; ++i) { packet[7 + i] = data[i]; checksum += data[i]; }
  packet[7 + length] = (uint8_t)(0 - checksum);
  packet[8 + length] = 0;
  if (wakePending_) {
    if (deadline.remaining() < 3) return fail(Pn532Fault::Budget, Pn532Result::Deadline);
    bus_.beginTransactionWithAssertingCS();
    delayMicroseconds(2000); // RTOS delay(2) can return before two full milliseconds.
    for (uint8_t i = 0; i < length + 9; ++i) bus_.transfer(packet[i]);
    bus_.endTransactionWithDeassertingCS();
    wakePending_ = false;
  } else if (!bus_.write(packet, length + 9)) return fail(Pn532Fault::BusIo);
  if (deadline.expired()) return fail(Pn532Fault::Budget, Pn532Result::Deadline);
  delay(1); // Existing SPI settling interval; included in the same deadline.
  Pn532Result result = waitReady(deadline, true);
  if (result != Pn532Result::Ok) return result;
  result = readAck(deadline);
  if (result != Pn532Result::Ok) return result;
  if (deadline.expired()) return fail(Pn532Fault::Budget, Pn532Result::Deadline);
  delay(1);
  result = waitReady(deadline, false);
  if (result != Pn532Result::Ok) return result;
  return readResponse(data[0] + 1, response, responseLength, deadline);
}

Pn532Result RevivalPn532::getFirmwareVersion(uint32_t &version, const Pn532Deadline &deadline)
{
  version = 0;
  const uint8_t request[] = {0x02};
  uint8_t response[62], length;
  Pn532Result result = command(request, sizeof(request), response, length, deadline.limited(100));
  if (result != Pn532Result::Ok) return result;
  if (length != 4 || response[0] != 0x32) return fail(Pn532Fault::UnexpectedResponse);
  for (uint8_t i = 0; i < 4; ++i) version = (version << 8) | response[i];
  return Pn532Result::Ok;
}

Pn532Result RevivalPn532::configureSam(const Pn532Deadline &deadline)
{
  const uint8_t request[] = {0x14, 1, 0x14, 1};
  uint8_t response[62], length;
  Pn532Result result = command(request, sizeof(request), response, length, deadline.limited(100));
  if (result != Pn532Result::Ok) return result;
  if (length) return fail(Pn532Fault::UnexpectedResponse);
  // Normalize even on cold MCU startup: the PN532/tag can remain powered while
  // MCU RAM (including dirty flags) resets. Preserve a known interrupted value;
  // otherwise use Type A 106kbps with reception CRC enabled (RxMode=0x80).
  if ((!sessionRxModeDirty_ && !sessionSectorDirty_) ||
      (sessionRxMode_ & 0xF7) != 0x80) sessionRxMode_ = 0x80;
  sessionRxModeDirty_ = true;
  sessionSectorDirty_ = true;
  return repairNtagI2cState(deadline);
}

Pn532Result RevivalPn532::setRetries(uint8_t retries, const Pn532Deadline &deadline)
{
  const uint8_t request[] = {0x32, 5, 0xFF, 1, retries};
  uint8_t response[62], length;
  Pn532Result result = command(request, sizeof(request), response, length, deadline.limited(100));
  return result != Pn532Result::Ok ? result : length ? fail(Pn532Fault::UnexpectedResponse) : result;
}

Pn532Result RevivalPn532::setGain(uint8_t gainCfg, const Pn532Deadline &deadline)
{
  const uint8_t request[] = {0x32, 0x0A, gainCfg, 0xF4, 0x3F, 0x11, 0x4D, 0x85, 0x61, 0x6F, 0x26, 0x62, 0x87};
  uint8_t response[62], length;
  Pn532Result result = command(request, sizeof(request), response, length, deadline.limited(100));
  return result != Pn532Result::Ok ? result : length ? fail(Pn532Fault::UnexpectedResponse) : result;
}

Pn532Result RevivalPn532::setRfField(bool enabled, const Pn532Deadline &deadline)
{
  const uint8_t request[] = {0x32, 1, (uint8_t)(enabled ? 1 : 0)};
  uint8_t response[62], length;
  Pn532Result result = command(request, sizeof(request), response, length, deadline.limited(100));
  return result != Pn532Result::Ok ? result : length ? fail(Pn532Fault::UnexpectedResponse) : result;
}

Pn532Result RevivalPn532::readTarget(uint8_t *uid, uint8_t &uidLength, const Pn532Deadline &deadline)
{
  target_ = 0;
  uidLength = 0;
  if (sessionSectorDirty_ || sessionRxModeDirty_) return fail(Pn532Fault::SessionState);
  const uint8_t request[] = {0x4A, 1, 0};
  uint8_t response[62], length;
  Pn532Result result = command(request, sizeof(request), response, length, deadline.limited(250));
  if (result != Pn532Result::Ok) return result;
  if (length == 1 && response[0] == 0) return Pn532Result::NoTarget;
  if (length < 6 || response[0] != 1 || response[1] != 1)
    return fail(Pn532Fault::TargetResponse);
  uint8_t count = response[5];
  if ((count != 4 && count != 7 && count != 10) || length < 6 + count)
    return fail(Pn532Fault::TargetResponse);
  // Optional ATS follows the UID. If present, its length byte includes itself.
  uint8_t baseLength = 6 + count;
  if (length != baseLength && (response[baseLength] == 0 ||
      length != baseLength + response[baseLength])) return fail(Pn532Fault::TargetResponse);
  uidLength = count;
  memcpy(uid, response + 6, count);
  memcpy(uid_, response + 6, count);
  uidLength_ = count;
  target_ = 1;
  return Pn532Result::Ok;
}

Pn532Result RevivalPn532::readPage7(uint8_t *data, const Pn532Deadline &deadline)
{
  uint8_t pages[16];
  Pn532Result result = readPages(7, pages, deadline);
  if (result == Pn532Result::Ok) memcpy(data, pages, 4);
  return result;
}

Pn532Result RevivalPn532::readPages(uint8_t page, uint8_t *data16, const Pn532Deadline &deadline)
{
  if (sessionSectorDirty_ || sessionRxModeDirty_) return fail(Pn532Fault::SessionState);
  if (!target_) return fail(Pn532Fault::TargetResponse);
  const uint8_t request[] = {0x40, target_, 0x30, page};
  uint8_t response[62], length;
  Pn532Result result = command(request, sizeof(request), response, length, deadline.limited(100));
  if (result != Pn532Result::Ok) return result;
  if (!length) return fail(Pn532Fault::PageResponse);
  tagStatus_ = response[0];
  if (tagStatus_ & 0x3F) return Pn532Result::TagError;
  if (tagStatus_ || length != 17) return fail(Pn532Fault::PageResponse);
  memcpy(data16, response + 1, 16);
  return Pn532Result::Ok;
}

Pn532Result RevivalPn532::getTagVersion(uint8_t *data8, const Pn532Deadline &deadline)
{
  if (sessionSectorDirty_ || sessionRxModeDirty_) return fail(Pn532Fault::SessionState);
  if (!target_) return fail(Pn532Fault::TargetResponse);
  // InDataExchange interprets 0x60 as MIFARE Authentication A and enforces
  // its key/UID length. Send NTAG GET_VERSION through the raw RF exchange;
  // InListPassiveTarget has already selected the target and configured CRC.
  const uint8_t request[] = {0x42, 0x60};
  uint8_t response[62], length;
  Pn532Result result = command(request, sizeof(request), response, length, deadline.limited(100));
  if (result != Pn532Result::Ok) return result;
  if (!length) return fail(Pn532Fault::PageResponse);
  tagStatus_ = response[0];
  if (tagStatus_ & 0x3F) return Pn532Result::TagError;
  if (tagStatus_ || length != 9) return fail(Pn532Fault::PageResponse);
  memcpy(data8, response + 1, 8);
  return Pn532Result::Ok;
}

Pn532Result RevivalPn532::writePage(uint8_t page, const uint8_t *data4, const Pn532Deadline &deadline)
{
  if (page < 4 || page > 39 || !data4) return fail(Pn532Fault::PageResponse, Pn532Result::TagError);
  if (sessionSectorDirty_ || sessionRxModeDirty_) return fail(Pn532Fault::SessionState);
  if (!target_) return fail(Pn532Fault::TargetResponse);
  const uint8_t request[] = {0x40, target_, 0xA2, page, data4[0], data4[1], data4[2], data4[3]};
  uint8_t response[62], length;
  Pn532Result result = command(request, sizeof(request), response, length, deadline.limited(100));
  if (result != Pn532Result::Ok) return result;
  if (!length) return fail(Pn532Fault::PageResponse);
  tagStatus_ = response[0];
  if (tagStatus_ & 0x3F) return Pn532Result::TagError;
  // InDataExchange consumes the Type 2 ACK; only its status byte is returned.
  // This is command acknowledgement, not end-to-end EEPROM verification.
  if (tagStatus_ || length != 1) return fail(Pn532Fault::PageResponse);
  return Pn532Result::Ok;
}

Pn532Result RevivalPn532::readRxMode(uint8_t &mode, const Pn532Deadline &deadline)
{
  const uint8_t request[] = {0x06, 0x63, 0x03};
  uint8_t response[62], length;
  recordSessionResponse(nullptr, 0);
  Pn532Result result = command(request, sizeof(request), response, length, deadline.limited(100));
  if (result != Pn532Result::Ok) return result;
  recordSessionResponse(response, length);
  if (length != 1) return fail(Pn532Fault::SessionResponse);
  mode = response[0];
  return Pn532Result::Ok;
}

Pn532Result RevivalPn532::writeRxMode(uint8_t mode, const Pn532Deadline &deadline)
{
  const uint8_t request[] = {0x08, 0x63, 0x03, mode};
  uint8_t response[62], length;
  recordSessionResponse(nullptr, 0);
  Pn532Result result = command(request, sizeof(request), response, length, deadline.limited(100));
  if (result == Pn532Result::Ok) recordSessionResponse(response, length);
  return result != Pn532Result::Ok ? result : length ? fail(Pn532Fault::SessionResponse) : result;
}

Pn532Result RevivalPn532::restoreSessionRxMode(const Pn532Deadline &deadline)
{
  Pn532Result result = writeRxMode(sessionRxMode_, deadline);
  if (result != Pn532Result::Ok) return result;
  uint8_t actual = 0;
  result = readRxMode(actual, deadline);
  if (result != Pn532Result::Ok) return result;
  if (actual != sessionRxMode_) return fail(Pn532Fault::SessionState);
  sessionRxModeDirty_ = false;
  return Pn532Result::Ok;
}

Pn532Result RevivalPn532::selectNtagI2cSector(uint8_t sector, const Pn532Deadline &deadline)
{
  // NXP AN10609_3 p37 shows disabling only reception CRC to receive a Type A
  // four-bit ACK via InCommunicateThru. No CRC-error status is accepted as ACK.
  // Mark dirty before sending: a lost host response can still mean it executed.
  sessionStep_ = sector == 3 ? "sector3-crc-off" : "sector0-crc-off";
  sessionRxModeDirty_ = true;
  Pn532Result result = writeRxMode(sessionRxMode_ & 0x7F, deadline);
  if (result != Pn532Result::Ok) return result;
  const uint8_t first[] = {0x42, 0xC2, 0xFF}; // Avoid InDataExchange MIFARE Restore.
  uint8_t response[62], length;
  sessionSectorDirty_ = true;
  sessionStep_ = sector == 3 ? "sector3-ack" : "sector0-ack";
  recordSessionResponse(nullptr, 0);
  result = command(first, sizeof(first), response, length, deadline.limited(100));
  if (result != Pn532Result::Ok) return result;
  recordSessionResponse(response, length);
  if (length) tagStatus_ = response[0];
  if (length != 2 || response[0] != 0 || response[1] != 0x0A)
    return fail(Pn532Fault::SessionResponse);
  sessionStep_ = sector == 3 ? "sector3-crc-restore" : "sector0-crc-restore";
  result = restoreSessionRxMode(deadline);
  if (result != Pn532Result::Ok) return result;
  const uint8_t second[] = {0x42, sector, 0, 0, 0};
  sessionStep_ = sector == 3 ? "sector3-passive" : "sector0-passive";
  recordSessionResponse(nullptr, 0);
  result = command(second, sizeof(second), response, length, deadline.limited(100));
  if (result != Pn532Result::Ok) return result;
  recordSessionResponse(response, length);
  if (length) tagStatus_ = response[0];
  // NT3H1101 §10.9: packet 2 succeeds by remaining silent. PN532's default
  // fRetryTimeout=0x0A (51.2ms), MaxRtyCOM=0 ends it with a validated status01.
  // A host timeout, status00/data reply, CRC error or extra byte is NOT success.
  if (length != 1 || response[0] != 1) return fail(Pn532Fault::SessionResponse);
  return Pn532Result::Ok;
}

Pn532Result RevivalPn532::readSessionPages(uint8_t page, uint8_t *data16,
                                        const Pn532Deadline &deadline)
{
  const uint8_t request[] = {0x40, target_, 0x30, page};
  uint8_t response[62], length;
  recordSessionResponse(nullptr, 0);
  Pn532Result result = command(request, sizeof(request), response, length, deadline.limited(100));
  if (result != Pn532Result::Ok) return result;
  recordSessionResponse(response, length);
  if (length) tagStatus_ = response[0];
  if (length != 17 || response[0] != 0) return fail(Pn532Fault::SessionResponse);
  memcpy(data16, response + 1, 16);
  return Pn532Result::Ok;
}

Pn532Result RevivalPn532::readNtagI2cSession(uint8_t *data8, const Pn532Deadline &budget)
{
  sessionStep_ = "precondition";
  recordSessionResponse(nullptr, 0);
  if (sessionSectorDirty_ || sessionRxModeDirty_) return fail(Pn532Fault::SessionState);
  if (!target_ || uidLength_ != 7 || !data8) return fail(Pn532Fault::TargetResponse);
  const Pn532Deadline deadline = budget.limited(250);
  uint8_t header[16], session[16], restored[16];
  sessionStep_ = "header";
  Pn532Result result = readSessionPages(0, header, deadline);
  if (result != Pn532Result::Ok) return result;
  // Original NT3H1101/1201 Rev3.3 §8.3.5/Fig6 stores UID0..UID6 in the
  // first seven consecutive bytes, then SAK/ATQA; it is not the 21x BCC layout.
  if (memcmp(header, uid_, 7))
    return fail(Pn532Fault::SessionResponse);
  sessionStep_ = "rxmode";
  result = readRxMode(sessionRxMode_, deadline);
  if (result != Pn532Result::Ok) return result;
  // CRC must initially be on, speed=106kbps, Type A, single reception. Preserve
  // the independent RxNoErr bit when changing only RxCRCEn (PN532/C1 §8.6.23.19).
  if ((sessionRxMode_ & 0xF7) != 0x80) return fail(Pn532Fault::SessionState);
  result = selectNtagI2cSector(3, deadline);
  if (result != Pn532Result::Ok) return result;
  sessionStep_ = "session-read";
  result = readSessionPages(0xF8, session, deadline);
  if (result != Pn532Result::Ok) return result;
  // F8/F9 are the eight session bytes; FA/FB are invalid and read as zero.
  sessionStep_ = "session-tail";
  for (uint8_t i = 8; i < 16; ++i)
    if (session[i]) return fail(Pn532Fault::SessionResponse);
  result = selectNtagI2cSector(0, deadline);
  if (result != Pn532Result::Ok) return result;
  sessionStep_ = "header-restore";
  result = readSessionPages(0, restored, deadline);
  if (result != Pn532Result::Ok) return result;
  if (memcmp(header, restored, sizeof(header))) return fail(Pn532Fault::SessionState);
  sessionSectorDirty_ = false;
  memcpy(data8, session, 8); // Publish only after proven sector/CRC restoration.
  sessionStep_ = "done";
  return Pn532Result::Ok;
}

Pn532Result RevivalPn532::repairNtagI2cState(const Pn532Deadline &deadline)
{
  if (!sessionRxModeDirty_ && !sessionSectorDirty_) return Pn532Result::Ok;
  target_ = 0;
  if (sessionRxModeDirty_) {
    sessionStep_ = "recovery-rxmode";
    Pn532Result result = restoreSessionRxMode(deadline);
    if (result != Pn532Result::Ok) return result;
  }
  // A complete RF cycle is required after a partial sector selection; merely
  // issuing RF ON does not remove an active tag's unknown selected sector.
  sessionSectorDirty_ = true;
  sessionStep_ = "recovery-rf-off";
  recordSessionResponse(nullptr, 0);
  if (deadline.remaining() <= 12) return fail(Pn532Fault::Budget, Pn532Result::Deadline);
  Pn532Result result = setRfField(false, deadline.limited(deadline.remaining() - 12));
  if (result != Pn532Result::Ok) return result;
  delayMicroseconds(6000);
  if (deadline.remaining() <= 6) return fail(Pn532Fault::Budget, Pn532Result::Deadline);
  sessionStep_ = "recovery-rf-on";
  result = setRfField(true, deadline.limited(deadline.remaining() - 6));
  if (result != Pn532Result::Ok) return result;
  delayMicroseconds(6000);
  sessionSectorDirty_ = false; // A fresh readTarget is still required.
  sessionStep_ = "recovered";
  return Pn532Result::Ok;
}

void RevivalPn532::recordSessionResponse(const uint8_t *data, uint8_t length)
{
  sessionResponseLength_ = length;
  sessionResponse_[0] = data && length ? data[0] : 0;
  sessionResponse_[1] = data && length > 1 ? data[1] : 0;
}

const char *RevivalPn532::faultName() const
{
  static const char *names[] = {"none", "status_flags", "invalid_status", "ack_timeout", "response_timeout",
    "bad_ack", "nack", "bad_frame", "frame_length", "checksum", "unexpected_response",
    "error_frame", "target_response", "page_response", "budget", "bus_io",
    "session_response", "session_state"};
  return names[(unsigned int)fault_];
}
const char *RevivalPn532::phaseName() const
{
  static const char *names[] = {"idle", "write", "ack_wait", "ack_read", "response_wait", "response_read"};
  return names[(unsigned int)phase_];
}
