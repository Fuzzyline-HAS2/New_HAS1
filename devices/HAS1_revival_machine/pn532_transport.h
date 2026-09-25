#ifndef REVIVAL_PN532_TRANSPORT_H
#define REVIVAL_PN532_TRANSPORT_H
#include <Arduino.h>
#include <Adafruit_SPIDevice.h>

enum class Pn532Result { Ok, NoTarget, TagError, TransportFault, Deadline };
enum class Pn532Fault { None, StatusFlags, InvalidStatus, AckTimeout, ResponseTimeout,
                       BadAck, Nack, BadFrame, FrameLength, Checksum, UnexpectedResponse,
                       ErrorFrame, TargetResponse, PageResponse, Budget, BusIo,
                       SessionResponse, SessionState };
enum class Pn532Phase { Idle, Write, AckWait, AckRead, ResponseWait, ResponseRead };
struct Pn532Deadline {
  uint32_t started_ms;
  uint32_t budget_ms;
  uint32_t remaining() const {
    uint32_t elapsed = (uint32_t)(millis() - started_ms);
    return elapsed >= budget_ms ? 0 : budget_ms - elapsed;
  }
  bool expired() const { return remaining() == 0; }
  Pn532Deadline limited(uint32_t maximum) const {
    uint32_t now = millis();
    uint32_t elapsed = (uint32_t)(now - started_ms);
    uint32_t left = elapsed >= budget_ms ? 0 : budget_ms - elapsed;
    return { now, left < maximum ? left : maximum };
  }
};

class RevivalPn532 {
public:
  RevivalPn532(uint8_t sck, uint8_t miso, uint8_t mosi, uint8_t ss);
  bool beginPins(); // No PN532 commands and no claim of chip readiness.
  Pn532Result wake(const Pn532Deadline &deadline); // Arm next command CS wake pulse; not a hardware reset.
  Pn532Result abort(const Pn532Deadline &deadline); // Host ACK only; no reply expected.
  Pn532Result getFirmwareVersion(uint32_t &version, const Pn532Deadline &deadline);
  Pn532Result configureSam(const Pn532Deadline &deadline);
  Pn532Result setRetries(uint8_t retries, const Pn532Deadline &deadline);
  Pn532Result setGain(uint8_t gainCfg, const Pn532Deadline &deadline);
  Pn532Result setRfField(bool enabled, const Pn532Deadline &deadline);
  Pn532Result readTarget(uint8_t *uid, uint8_t &uidLength, const Pn532Deadline &deadline);
  Pn532Result readPage7(uint8_t *data, const Pn532Deadline &deadline);
  Pn532Result readPages(uint8_t page, uint8_t *data16, const Pn532Deadline &deadline);
  Pn532Result getTagVersion(uint8_t *data8, const Pn532Deadline &deadline);
  // Original NTAG I2C: sector 3 F8/F9, restored and verified in sector 0 before
  // publishing eight bytes. Whole operation <=250ms. Failure requires recovery:
  // configureSam repairs pending CRC/sector state before card APIs can resume.
  Pn532Result readNtagI2cSession(uint8_t *data8, const Pn532Deadline &deadline);
  // Deliberately limited to the shared 144-byte user region of NTAG213/215/216.
  Pn532Result writePage(uint8_t page, const uint8_t *data4, const Pn532Deadline &deadline);
  Pn532Fault lastFault() const { return fault_; }
  Pn532Phase lastPhase() const { return phase_; }
  uint8_t lastStatus() const { return status_; }
  uint8_t lastTagStatus() const { return tagStatus_; }
  const char *lastSessionStep() const { return sessionStep_; }
  uint8_t lastSessionResponseLength() const { return sessionResponseLength_; }
  uint8_t lastSessionResponseByte(uint8_t index) const { return index < 2 ? sessionResponse_[index] : 0; }
  const char *faultName() const;
  const char *phaseName() const;
private:
  Adafruit_SPIDevice bus_;
  Pn532Fault fault_ = Pn532Fault::None;
  Pn532Phase phase_ = Pn532Phase::Idle;
  uint8_t status_ = 0;
  uint8_t tagStatus_ = 0;
  uint8_t target_ = 0;
  uint8_t uid_[10] = {};
  uint8_t uidLength_ = 0;
  bool wakePending_ = false;
  bool sessionSectorDirty_ = false;
  bool sessionRxModeDirty_ = false;
  uint8_t sessionRxMode_ = 0;
  const char *sessionStep_ = "idle";
  uint8_t sessionResponseLength_ = 0;
  uint8_t sessionResponse_[2] = {};
  Pn532Result fail(Pn532Fault fault, Pn532Result result = Pn532Result::TransportFault);
  Pn532Result waitReady(const Pn532Deadline &deadline, bool ack);
  Pn532Result readAck(const Pn532Deadline &deadline);
  Pn532Result readResponse(uint8_t expected, uint8_t *data, uint8_t &length,
                          const Pn532Deadline &deadline);
  Pn532Result command(const uint8_t *data, uint8_t length, uint8_t *response,
                      uint8_t &responseLength, const Pn532Deadline &deadline);
  Pn532Result readRxMode(uint8_t &mode, const Pn532Deadline &deadline);
  Pn532Result writeRxMode(uint8_t mode, const Pn532Deadline &deadline);
  Pn532Result restoreSessionRxMode(const Pn532Deadline &deadline);
  Pn532Result selectNtagI2cSector(uint8_t sector, const Pn532Deadline &deadline);
  Pn532Result readSessionPages(uint8_t page, uint8_t *data16, const Pn532Deadline &deadline);
  Pn532Result repairNtagI2cState(const Pn532Deadline &deadline);
  void recordSessionResponse(const uint8_t *data, uint8_t length);
};
#endif
