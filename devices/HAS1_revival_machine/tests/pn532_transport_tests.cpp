// Real device-local transport compiled against a byte-level SPI/clock fake.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <vector>
#include "pn532_transport.h"

using Bytes = std::vector<uint8_t>;
static uint64_t nowUs = 1000000;
uint32_t millis() { return static_cast<uint32_t>(nowUs / 1000); }
uint32_t micros() { return static_cast<uint32_t>(nowUs); }
void delay(unsigned long ms) { nowUs += uint64_t(ms) * 1000; }
void delayMicroseconds(unsigned int us) { nowUs += us; }

static Bytes frame(Bytes payload) {
  assert(payload.size() < 255);
  const uint8_t len = payload.size();
  Bytes result{0, 0, 0xFF, len, static_cast<uint8_t>(-len)};
  result.insert(result.end(), payload.begin(), payload.end());
  uint8_t sum = 0;
  for (auto byte : payload) sum += byte;
  result.push_back(static_cast<uint8_t>(-sum)); result.push_back(0);
  return result;
}
struct Exchange {
  Bytes command;
  Bytes response;
  Bytes ack = {0, 0, 0xFF, 0, 0xFF, 0};
  uint32_t ackDelayMs = 0, responseDelayMs = 0;
  uint8_t readyStatus = 1, responseStatus = 1;
};
static std::deque<Exchange> planned;
static Exchange active;
static bool activeSet = false, responsePhase = false;
static uint64_t readyUs = 0;
static size_t readOffset = 0, maxRead = 0;
static unsigned commands = 0, aborts = 0, statusReads = 0;
static bool selected = false, readCommand = false, writeCommand = false;
static Bytes transactionWrite;
static uint64_t selectedAtUs = 0, writeWakeUs = 0;
bool TestSpiWrite(const uint8_t*, size_t, const uint8_t*, size_t);
static size_t transactionRead = 0;
static uint8_t idleStatus = 0;
static bool beginOk = true, writesOk = true, readsOk = true;
static unsigned transferDelayUs = 0;
static std::vector<Bytes> sentCommands;

bool TestSpiBegin() { return beginOk; }
void TestSpiTransactionBegin() {
  assert(!selected); selected = true; readCommand = false; writeCommand = false;
  transactionRead = 0; transactionWrite.clear(); selectedAtUs = nowUs;
}
void TestSpiTransactionEnd() {
  assert(selected); selected = false; maxRead = std::max(maxRead, transactionRead);
  if (writeCommand) {
    TestSpiWrite(transactionWrite.data(), transactionWrite.size(), nullptr, 0); return;
  }
  if (!readCommand) return; // Wake uses chip select without FIFO reads.
  const Bytes& bytes = responsePhase ? active.response : active.ack;
  if (readOffset == bytes.size()) {
    if (!responsePhase) { responsePhase = true; readyUs = nowUs + uint64_t(active.responseDelayMs) * 1000; }
    else activeSet = false;
    readOffset = 0;
  }
}
uint8_t TestSpiTransfer(uint8_t value) {
  assert(selected); nowUs += transferDelayUs;
  if (writeCommand) { transactionWrite.push_back(value); return 0; }
  if (!readCommand) {
    if (value == 1) { writeCommand = true; transactionWrite.push_back(value); writeWakeUs = nowUs - selectedAtUs; return 0; }
    assert(value == 3); readCommand = true; return 0;
  }
  assert(activeSet && nowUs >= readyUs);
  const Bytes& bytes = responsePhase ? active.response : active.ack;
  assert(readOffset < bytes.size() && "Production transport overread FIFO");
  ++transactionRead;
  return bytes[readOffset++];
}
bool TestSpiWriteRead(const uint8_t* data, size_t size, uint8_t* out, size_t outSize) {
  assert(size == 1);
  if (data[0] == 3) {
    if (!readsOk) return false;
    TestSpiTransactionBegin(); TestSpiTransfer(3);
    for (size_t i = 0; i < outSize; ++i) out[i] = TestSpiTransfer(0xFF);
    TestSpiTransactionEnd(); return true;
  }
  assert(outSize == 1 && data[0] == 2);
  ++statusReads;
  if (!readsOk) return false;
  nowUs += transferDelayUs;
  *out = activeSet ? (nowUs >= readyUs ? (responsePhase ? active.responseStatus : active.readyStatus) : 0) : idleStatus;
  return true;
}
bool TestSpiWrite(const uint8_t* data, size_t size, const uint8_t* prefix, size_t prefixSize) {
  Bytes bytes;
  if (prefixSize) bytes.insert(bytes.end(), prefix, prefix + prefixSize);
  bytes.insert(bytes.end(), data, data + size);
  if (!writesOk) return false;
  assert(bytes.size() >= 7 && bytes[0] == 1);
  bytes.erase(bytes.begin());
  if (bytes == Bytes{0, 0, 0xFF, 0, 0xFF, 0}) {
    ++aborts; activeSet = false; return true;
  }
  assert(bytes.size() >= 9 && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0xFF);
  assert(static_cast<uint8_t>(bytes[3] + bytes[4]) == 0);
  assert(bytes.size() == size_t(bytes[3]) + 7 && bytes[5] == 0xD4 && bytes.back() == 0);
  uint8_t sum = 0;
  for (size_t i = 5; i + 1 < bytes.size(); ++i) sum += bytes[i];
  assert(sum == 0);
  Bytes command(bytes.begin() + 6, bytes.end() - 2);
  sentCommands.push_back(command); ++commands;
  assert(!planned.empty() && "Unexpected production command");
  active = planned.front(); planned.pop_front();
  assert(command == active.command);
  activeSet = true; responsePhase = false; readOffset = 0;
  readyUs = nowUs + uint64_t(active.ackDelayMs) * 1000;
  return true;
}

static void expect(bool condition, const char* message) {
  if (!condition) { std::cerr << message << '\n'; std::abort(); }
}
static void queue(Bytes command, Bytes payload) { planned.push_back({command, frame(payload)}); }
static Bytes foundTarget() { return {0xD5, 0x4B, 1, 1, 0x04, 0x00, 0x00, 7, 1, 2, 3, 4, 5, 6, 7}; }
static Bytes pageData() {
  Bytes payload{0xD5, 0x41, 0};
  const uint8_t data[16] = {'G', '9', 'P', '2', 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  payload.insert(payload.end(), data, data + 16); return payload;
}

static Bytes i2cHeader() {
  // Original NT3H1101 Rev3.3 Fig6: UID0..6, SAK, ATQA, static lock, CC.
  // Actual connected card UID; byte3=0x52 is not an NTAG21x cascade BCC.
  return {0x04, 0x09, 0x6F, 0x52, 0x78, 0x74, 0x80, 0, 0x44, 0, 0, 0, 0xE1, 0x10, 0x6D, 0};
}
static Bytes foundI2cTarget() {
  Bytes payload = foundTarget();
  Bytes header = i2cHeader();
  std::copy(header.begin(), header.begin() + 7, payload.begin() + 8);
  return payload;
}
static Bytes readPayload(const Bytes& data) {
  Bytes payload{0xD5, 0x41, 0};
  payload.insert(payload.end(), data.begin(), data.end()); return payload;
}
static void queueSector(uint8_t sector, uint8_t mode) {
  queue({0x08, 0x63, 0x03, static_cast<uint8_t>(mode & 0x7F)}, {0xD5, 0x09});
  queue({0x42, 0xC2, 0xFF}, {0xD5, 0x43, 0, 0x0A});
  queue({0x08, 0x63, 0x03, mode}, {0xD5, 0x09});
  queue({0x06, 0x63, 0x03}, {0xD5, 0x07, mode});
  queue({0x42, sector, 0, 0, 0}, {0xD5, 0x43, 1});
  planned.back().responseDelayMs = 52; // Model PN532's documented default RF timeout (51.2ms).
}
static void queueSession(uint8_t mode = 0x80) {
  queue({0x40, 1, 0x30, 0}, readPayload(i2cHeader()));
  queue({0x06, 0x63, 0x03}, {0xD5, 0x07, mode});
  queueSector(3, mode);
  queue({0x40, 1, 0x30, 0xF8}, readPayload({1, 0, 0, 0x48, 8, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
  queueSector(0, mode);
  queue({0x40, 1, 0x30, 0}, readPayload(i2cHeader()));
}
static void queueSamRepair(uint8_t mode = 0x80) {
  queue({0x14, 1, 0x14, 1}, {0xD5, 0x15});
  queue({0x08, 0x63, 0x03, mode}, {0xD5, 0x09});
  queue({0x06, 0x63, 0x03}, {0xD5, 0x07, mode});
  queue({0x32, 1, 0}, {0xD5, 0x33});
  queue({0x32, 1, 1}, {0xD5, 0x33});
}
static void expectSessionBlocked(RevivalPn532& reader) {
  const auto previousCommands = commands;
  uint8_t output[16] = {}, uid[10] = {}, length = 0;
  expect(reader.readTarget(uid, length, {millis(), 100}) == Pn532Result::TransportFault,
         "Dirty sector/CRC must block UID re-selection until recovery");
  expect(reader.readPages(4, output, {millis(), 100}) == Pn532Result::TransportFault,
         "Dirty sector/CRC must block all page reads");
  expect(reader.getTagVersion(output, {millis(), 100}) == Pn532Result::TransportFault,
         "Dirty sector/CRC must block raw tag version reads");
  expect(reader.writePage(7, output, {millis(), 100}) == Pn532Result::TransportFault,
         "Dirty sector/CRC must block writes even to an otherwise allowed page");
  expect(reader.readNtagI2cSession(output, {millis(), 250}) == Pn532Result::TransportFault,
         "Dirty sector/CRC must block another session until recovery");
  expect(commands == previousCommands, "Blocked operations must perform no SPI commands");
}

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string scenario = argv[1];
  RevivalPn532 reader(18, 19, 23, 5);
  reader.beginPins();
  uint8_t uid[10] = {}, length = 0, data[32];
  memset(data, 0xCC, sizeof(data));
  uint32_t version = 0;
  Pn532Result result = Pn532Result::TransportFault;
  if (scenario.find("session_") == 0) {
    if (scenario == "session_no_target") {
      expect(reader.readNtagI2cSession(data, {millis(), 250}) == Pn532Result::TransportFault && commands == 0,
             "Session reads require a selected seven-byte target before any SPI operation");
    } else {
      if (scenario == "session_wrap") nowUs = (uint64_t(UINT32_MAX) - 60) * 1000;
      queue({0x4A, 1, 0}, foundI2cTarget());
      assert(reader.readTarget(uid, length, {millis(), 100}) == Pn532Result::Ok);
      if (scenario == "session_null") {
        expect(reader.readNtagI2cSession(nullptr, {millis(), 250}) == Pn532Result::TransportFault && commands == 1,
               "Null session output must be rejected before SPI access");
      } else {
        uint8_t mode = (scenario == "session_rxmode_preserved" || scenario == "session_recovery") ? 0x88 : 0x80;
        queueSession(mode);
        int stop = -1;
        bool successful = scenario == "session_ok" || scenario == "session_rxmode_preserved" || scenario == "session_wrap";
        if (scenario.find("session_uid_byte_") == 0) {
          const unsigned index = scenario.back() - '0';
          expect(index < 7, "UID mutation index must be within seven-byte serial number");
          Bytes header = i2cHeader(); header[index] ^= 1;
          planned[0].response = frame(readPayload(header)); stop = 0;
        } else if (scenario == "session_21x_header_rejected") {
          // The old incorrect fixture includes cascade BCC0/1 and a UID gap.
          planned[0].response = frame(readPayload({0x04,0x09,0x6F,0xEA,0x52,0x78,0x74,0x80,
                                                 0xDE,0x48,0,0,0xE1,0x10,0x6D,0}));
          stop = 0;
        } else if (scenario == "session_rxmode_invalid") {
          planned[1].response = frame({0xD5, 0x07, 0}); stop = 1;
        } else if (scenario == "session_disable_timeout") {
          planned[2].responseDelayMs = 150; stop = 2;
        } else if (scenario == "session_ack_crc" || scenario == "session_recovery" || scenario == "session_recovery_bad_readback") {
          planned[3].response = frame({0xD5, 0x43, 2}); stop = 3;
        } else if (scenario == "session_ack_nak") {
          planned[3].response = frame({0xD5, 0x43, 0, 0}); stop = 3;
        } else if (scenario == "session_ack_extra") {
          planned[3].response = frame({0xD5, 0x43, 0, 0x0A, 0}); stop = 3;
        } else if (scenario == "session_ack_checksum") {
          planned[3].response[planned[3].response.size() - 2] ^= 1; stop = 3;
        } else if (scenario == "session_restore_rx_timeout") {
          planned[4].responseDelayMs = 150; stop = 4;
        } else if (scenario == "session_restore_rx_mismatch") {
          planned[5].response = frame({0xD5, 0x07, 0}); stop = 5;
        } else if (scenario == "session_packet2_host_timeout") {
          planned[6].responseDelayMs = 150; stop = 6;
        } else if (scenario == "session_packet2_reply") {
          planned[6].response = frame({0xD5, 0x43, 0}); stop = 6;
        } else if (scenario == "session_packet2_crc") {
          planned[6].response = frame({0xD5, 0x43, 2}); stop = 6;
        } else if (scenario == "session_packet2_extra") {
          planned[6].response = frame({0xD5, 0x43, 1, 0x0A}); stop = 6;
        } else if (scenario == "session_read_short") {
          planned[7].response = frame(readPayload(Bytes(8, 0))); stop = 7;
        } else if (scenario == "session_read_error") {
          planned[7].response = frame({0xD5, 0x41, 1}); stop = 7;
        } else if (scenario == "session_read_nonzero_tail") {
          Bytes value(16, 0); value[8] = 1;
          planned[7].response = frame(readPayload(value)); stop = 7;
        } else if (scenario == "session_sector0_timeout") {
          planned[12].responseDelayMs = 150; stop = 12;
        } else if (scenario == "session_header_changed") {
          Bytes header = i2cHeader(); header[15] ^= 1;
          planned[13].response = frame(readPayload(header)); stop = 13;
        } else if (scenario == "session_total_budget") {
          for (auto& exchange : planned) exchange.ackDelayMs = 20;
        } else expect(successful, "Unknown session scenario");
        if (stop >= 0) planned.resize(static_cast<size_t>(stop) + 1);
        const auto started = nowUs;
        result = reader.readNtagI2cSession(data, {millis(), 1000}); // Adapter must cap whole operation to 250ms.
        expect(nowUs - started <= 250000, "All session commands must share one 250ms wrapping deadline");
        if (successful) {
          expect(result == Pn532Result::Ok && commands == 15 && !activeSet,
                 "Session must validate all fourteen PN operations before success");
          expect(data[0] == 1 && data[3] == 0x48 && data[6] == 1 && data[7] == 0 && data[8] == 0xCC,
                 "Session publishes exactly eight bytes after sector and CRC restoration");
          expect(nowUs - started >= 104000, "Two passive ACKs consume independent RF timeout waits");
          expect(!strcmp(reader.lastSessionStep(), "done") && reader.lastSessionResponseLength() == 17 &&
                 reader.lastSessionResponseByte(0) == 0 && reader.lastSessionResponseByte(1) == 0x04,
                 "Completed session diagnostics retain validated header response shape without dumping UID");
          queue({0x40, 1, 0x30, 7}, pageData());
          expect(reader.readPage7(data, {millis(), 100}) == Pn532Result::Ok,
                 "Successful session leaves original selected target usable in sector zero");
        } else {
          expect(result == Pn532Result::TransportFault || result == Pn532Result::Deadline,
                 "Session errors require recovery; RF/host errors are never successful absence");
          expect(data[0] == 0xCC && data[7] == 0xCC && data[8] == 0xCC,
                 "Partially read session data must never be published on failure");
          if (stop >= 0) {
            const char *steps[] = {"header", "rxmode", "sector3-crc-off", "sector3-ack",
              "sector3-crc-restore", "sector3-crc-restore", "sector3-passive", "session-read",
              "sector0-crc-off", "sector0-ack", "sector0-crc-restore", "sector0-crc-restore",
              "sector0-passive", "header-restore"};
            const char *step = scenario == "session_read_nonzero_tail" ? "session-tail" : steps[stop];
            expect(!strcmp(reader.lastSessionStep(), step), "Session error must retain the exact failing logical step");
            if (stop == 0) expect(reader.lastSessionResponseLength() == 17 && reader.lastSessionResponseByte(0) == 0,
                                  "UID/header mismatch must distinguish valid RF response from host timeout");
            if (scenario == "session_ack_crc")
              expect(reader.lastSessionResponseLength() == 1 && reader.lastSessionResponseByte(0) == 2,
                     "Card CRC error diagnostics must retain actual status02 payload");
            if (scenario == "session_packet2_host_timeout" || scenario == "session_ack_checksum")
              expect(reader.lastSessionResponseLength() == 0,
                     "Missing/unvalidated response must not reuse the preceding response bytes");
          }
          if (scenario == "session_total_budget") {
            expect(result == Pn532Result::Deadline && nowUs - started == 250000,
                   "Session total bound is not a fresh per-command budget");
            planned.clear();
          }
          if (stop >= 2 || scenario == "session_total_budget") expectSessionBlocked(reader);
          if (scenario == "session_recovery" || scenario == "session_recovery_bad_readback") {
            queueSamRepair(mode);
            if (scenario == "session_recovery_bad_readback") {
              planned[2].response = frame({0xD5, 0x07, 0}); planned.resize(3);
              expect(reader.configureSam({millis(), 450}) == Pn532Result::TransportFault,
                     "Recovery must verify RxMode restoration rather than trusting write ACK");
              expectSessionBlocked(reader);
            } else {
              const auto recoveryStarted = nowUs;
              expect(reader.configureSam({millis(), 450}) == Pn532Result::Ok,
                     "SAM recovery must restore preserved RxMode and cycle RF");
              expect(nowUs - recoveryStarted >= 12000, "Recovery must include both six-millisecond RF guards");
              queue({0x4A, 1, 0}, foundTarget());
              expect(reader.readTarget(uid, length, {millis(), 100}) == Pn532Result::Ok,
                     "Recovered reader must require and permit fresh UID activation");
              queue({0x40, 1, 0x30, 7}, pageData());
              expect(reader.readPage7(data, {millis(), 100}) == Pn532Result::Ok,
                     "Recovered sector/CRC state must permit ordinary gameplay reads");
            }
          }
        }
      }
    }
  } else if (scenario == "abort" || scenario == "abort_budget") {
    const auto started = nowUs;
    result = reader.abort({millis(), scenario == "abort" ? 100U : 2U});
    if (scenario == "abort") {
      expect(result == Pn532Result::Ok && aborts == 1 && commands == 0 && statusReads == 0,
             "Host ACK abort must not consume a response or issue another command");
      expect(writeWakeUs >= 2000 && nowUs - started >= 2000, "Abort must hold CS through wake and ACK frame");
    } else expect(result == Pn532Result::Deadline && aborts == 0 && nowUs == started,
                  "Insufficient abort budget must not start a partial command");
  } else if (scenario == "no_target") {
    queue({0x4A, 1, 0}, {0xD5, 0x4B, 0});
    result = reader.readTarget(uid, length, {millis(), 250});
    expect(result == Pn532Result::NoTarget && length == 0, "Valid zero-target response must be absence");
    expect(!activeSet && commands == 1, "No-target response must be fully consumed");
  } else if (scenario == "target_uid10") {
    Bytes payload = foundTarget(); payload[7] = 10;
    payload.insert(payload.end(), {8, 9, 10});
    queue({0x4A, 1, 0}, payload);
    result = reader.readTarget(uid, length, {millis(), 100});
    expect(result == Pn532Result::Ok && length == 10 && uid[9] == 10, "Ten-byte UID must fit without overflow");
  } else if (scenario == "wake_sam") {
    Pn532Deadline deadline{millis(), 100};
    expect(reader.wake(deadline) == Pn532Result::Ok && commands == 0, "Wake must arm the next command without extra FIFO writes");
    queueSamRepair();
    expect(reader.configureSam(deadline) == Pn532Result::Ok, "SAM initialization failed");
    expect(writeWakeUs >= 2000 && commands == 5, "Wake/SAM shares CS, then CRC/readback and RF cycle normalize cold state");
  } else if (scenario == "target") {
    queue({0x4A, 1, 0}, foundTarget());
    result = reader.readTarget(uid, length, {millis(), 250});
    expect(result == Pn532Result::Ok && length == 7 && uid[0] == 1 && uid[6] == 7, "Target UID mismatch");
  } else if (scenario == "page") {
    queue({0x4A, 1, 0}, foundTarget());
    assert(reader.readTarget(uid, length, {millis(), 100}) == Pn532Result::Ok);
    queue({0x40, 1, 0x30, 7}, pageData());
    result = reader.readPage7(data, {millis(), 100});
    expect(result == Pn532Result::Ok && memcmp(data, "G9P2", 4) == 0, "Page payload mismatch");
    expect(!activeSet, "Four-page READ frame must be fully consumed once");
    expect(data[16] == 0xCC, "Page reader wrote outside the returned data");
  } else if (scenario == "upload_write_bounds" || scenario == "upload_write_no_target") {
    const uint8_t value[] = {'G', '1', 'P', '1'};
    if (scenario == "upload_write_no_target") {
      expect(reader.writePage(7, value, {millis(), 100}) == Pn532Result::TransportFault,
             "Writing without selected target must fail before SPI");
    } else {
      for (uint8_t page : {0, 1, 2, 3, 40, 41, 130, 226, 255})
        expect(reader.writePage(page, value, {millis(), 100}) == Pn532Result::TagError,
               "UID, CC, lock/config and unsupported pages must never be written");
    }
    expect(commands == 0, "Rejected write must not access card");
  } else if (scenario == "upload_read" || scenario == "tag_version" || scenario == "tag_version_short" ||
             scenario == "upload_write" || scenario == "upload_write_error" ||
             scenario == "upload_write_extra" || scenario == "upload_write_timeout") {
    queue({0x4A, 1, 0}, foundTarget());
    assert(reader.readTarget(uid, length, {millis(), 100}) == Pn532Result::Ok);
    if (scenario == "upload_read") {
      queue({0x40, 1, 0x30, 36}, pageData());
      result = reader.readPages(36, data, {millis(), 100});
      expect(result == Pn532Result::Ok && data[15] == 15 && data[16] == 0xCC,
             "READ must return exactly 16 bytes for requested address");
    } else if (scenario == "tag_version" || scenario == "tag_version_short") {
      Bytes response{0xD5, 0x43, 0, 0, 4, 4, 2, 1, 0, 0x0F, 3};
      if (scenario == "tag_version_short") response.pop_back();
      queue({0x42, 0x60}, response); // Avoid InDataExchange's MIFARE auth opcode collision.
      result = reader.getTagVersion(data, {millis(), 100});
      if (scenario == "tag_version")
        expect(result == Pn532Result::Ok && data[6] == 0x0F && data[8] == 0xCC,
               "GET_VERSION must return exactly 8 bytes without confusing PN532 firmware version");
      else expect(result == Pn532Result::TransportFault && data[0] == 0xCC,
                  "Truncated tag identification must not be accepted");
    } else {
      Bytes response{0xD5, 0x41, 0};
      if (scenario == "upload_write_error") response[2] = 1;
      if (scenario == "upload_write_extra") response.push_back(0x0A);
      queue({0x40, 1, 0xA2, 7, 'G', '1', 'P', '1'}, response);
      if (scenario == "upload_write_timeout") planned.back().responseDelayMs = 120;
      result = reader.writePage(7, reinterpret_cast<const uint8_t*>("G1P1"), {millis(), 100});
      const auto expected = scenario == "upload_write" ? Pn532Result::Ok :
          scenario == "upload_write_error" ? Pn532Result::TagError :
          scenario == "upload_write_timeout" ? Pn532Result::Deadline : Pn532Result::TransportFault;
      expect(result == expected, "Card WRITE needs complete validated PN532 status, not just host ACK");
    }
  } else if (scenario == "version") {
    queue({2}, {0xD5, 3, 0x32, 1, 6, 7});
    result = reader.getFirmwareVersion(version, {millis(), 100});
    expect(result == Pn532Result::Ok && version == 0x32010607, "Firmware version mismatch");
  } else if (scenario == "config_drains") {
    queue({0x32, 5, 0xFF, 1, 10}, {0xD5, 0x33});
    expect(reader.setRetries(10, {millis(), 100}) == Pn532Result::Ok && !activeSet,
           "RF retry response must be consumed before next command");
    queue({0x32, 1, 0}, {0xD5, 0x33});
    expect(reader.setRfField(false, {millis(), 100}) == Pn532Result::Ok && !activeSet,
           "RF OFF response must be consumed");
    queue({0x32, 1, 1}, {0xD5, 0x33});
    expect(reader.setRfField(true, {millis(), 100}) == Pn532Result::Ok && !activeSet,
           "RF ON response must be consumed");
  } else if (scenario == "status_overrun" || scenario == "status_tx_empty" || scenario == "response_status_fault" || scenario == "status_reserved") {
    queue({2}, {0xD5, 3, 0x32, 1, 6, 7});
    if (scenario == "response_status_fault") planned.back().responseStatus = 5;
    else planned.back().readyStatus = scenario == "status_overrun" ? 5 : scenario == "status_reserved" ? 0x81 : 12;
    const auto started = nowUs;
    result = reader.getFirmwareVersion(version, {millis(), 100});
    expect(result == Pn532Result::TransportFault && version == 0, "SPI exception is a transport fault, never ordinary ready/absence");
    expect(nowUs - started < 10000 && transactionRead == (scenario == "response_status_fault" ? 6 : 0), "SPI exception should fail before FIFO consumption or long wait");
  } else if (scenario == "ack_timeout" || scenario == "response_timeout" || scenario == "cumulative_budget" || scenario == "wrap_budget") {
    if (scenario == "wrap_budget") nowUs = (uint64_t(UINT32_MAX) - 20) * 1000;
    queue({2}, {0xD5, 3, 0x32, 1, 6, 7});
    planned.back().ackDelayMs = scenario == "ack_timeout" ? 150 : 40;
    planned.back().responseDelayMs = scenario == "response_timeout" ? 150 : 80;
    const auto started = nowUs;
    result = reader.getFirmwareVersion(version, {millis(), 100});
    expect(result == Pn532Result::Deadline && version == 0, "Exceeded cumulative exchange deadline must fail");
    expect(nowUs - started >= 100000 && nowUs - started <= 102000, "ACK and response must share one bounded, wrap-safe deadline");
  } else if (scenario == "expired") {
    Pn532Deadline deadline{millis(), 10}; delay(10);
    result = reader.getFirmwareVersion(version, deadline);
    expect(result == Pn532Result::Deadline && commands == 0 && statusReads == 0, "Expired deadline must not start SPI work");
  } else if (scenario == "limited_budget") {
    Pn532Deadline whole{millis(), 100}; delay(70);
    auto limited = whole.limited(50);
    expect(limited.remaining() <= 30, "Child budget must not extend its parent's remaining time");
    delay(30);
    expect(whole.expired() && limited.expired(), "Child and parent deadlines must expire together here");
  } else if (scenario == "tag_error" || scenario == "page_short" || scenario == "page_continuation") {
    queue({0x4A, 1, 0}, foundTarget());
    assert(reader.readTarget(uid, length, {millis(), 100}) == Pn532Result::Ok);
    Bytes payload = scenario == "tag_error" ? Bytes{0xD5, 0x41, 1} : pageData();
    if (scenario == "page_short") payload.pop_back();
    if (scenario == "page_continuation") payload[2] = 0x40;
    queue({0x40, 1, 0x30, 7}, payload);
    result = reader.readPage7(data, {millis(), 100});
    expect(result != Pn532Result::Ok && result != Pn532Result::NoTarget,
           "Payload/status errors must never become a successful read or physical absence");
    expect(data[0] == 0xCC, "Rejected page response must not publish its data");
  } else if (scenario == "target_short" || scenario == "target_uid_oversize" || scenario == "target_count") {
    Bytes payload = foundTarget();
    if (scenario == "target_short") payload.pop_back();
    if (scenario == "target_uid_oversize") payload[7] = 11;
    if (scenario == "target_count") payload[2] = 2;
    queue({0x4A, 1, 0}, payload);
    result = reader.readTarget(uid, length, {millis(), 100});
    expect(result != Pn532Result::Ok && result != Pn532Result::NoTarget && length == 0,
           "Malformed target must not publish UID or count as physical absence");
  } else if (scenario == "write_failure" || scenario == "status_io_failure") {
    if (scenario == "write_failure") writesOk = false;
    else { readsOk = false; queue({2}, {0xD5, 3, 0x32, 1, 6, 7}); }
    result = reader.getFirmwareVersion(version, {millis(), 100});
    expect(result == Pn532Result::TransportFault && version == 0, "SPI API failure must propagate");
  } else {
    queue({2}, {0xD5, 3, 0x32, 1, 6, 7});
    Exchange& pending = planned.back();
    if (scenario == "nack") pending.ack = {0, 0, 0xFF, 0xFF, 0, 0};
    else if (scenario == "ack_invalid") pending.ack = {0, 0, 0xFF, 3, 0xFD, 0xD5};
    else if (scenario == "response_preamble") pending.response[1] = 1;
    else if (scenario == "response_lcs") pending.response[4] ^= 1;
    else if (scenario == "response_checksum") pending.response[pending.response.size() - 2] ^= 1;
    else if (scenario == "response_postamble") pending.response.back() = 1;
    else if (scenario == "response_tfi") pending.response = frame({0xD4, 3, 0x32, 1, 6, 7});
    else if (scenario == "response_command") pending.response = frame({0xD5, 0x4B, 0x32, 1, 6, 7});
    else if (scenario == "response_error") pending.response = frame({0x7F});
    else if (scenario == "response_oversize") pending.response = {0, 0, 0xFF, 0xFE, 2};
    else if (scenario == "response_extended") pending.response = {0, 0, 0xFF, 0xFF, 0xFF};
    else assert(false && "Unknown scenario");
    result = reader.getFirmwareVersion(version, {millis(), 100});
    expect(result == Pn532Result::TransportFault && version == 0, "Invalid ACK/frame must be rejected");
    expect(maxRead <= 64, "Malformed response must not trigger an unbounded FIFO read");
  }
  expect(planned.empty(), "Expected command not issued");
  expect(!selected, "SPI CS must be released on every return");
  std::cout << "PASS " << scenario << '\n';
}
