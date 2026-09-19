#include "BeaconState.h"

#include <string.h>

namespace Has1BleBeacon {
namespace {
constexpr uint16_t kReset = 0x0c03;
constexpr uint16_t kParameters = 0x2006;
constexpr uint16_t kAdvertisingData = 0x2008;
constexpr uint16_t kScanResponse = 0x2009;
constexpr uint16_t kEnable = 0x200a;
constexpr uint16_t kIntervalUnits = 400;  // 250 ms, plus BLE advertising delay.

bool validName(const char* name) {
  if (name == nullptr || name[0] == '\0') return false;
  for (size_t i = 0; i <= kMaxDeviceName; ++i) {
    const char c = name[i];
    if (c == '\0') return true;
    if (i == kMaxDeviceName) return false;
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  }
  return false;
}
}  // namespace

bool decodeCommandEvent(const uint8_t* data, size_t size,
                        uint16_t& opcode, uint8_t& status) {
  if (data == nullptr || size < 7 || data[0] != 0x04 ||
      data[2] < 4 || size < static_cast<size_t>(data[2]) + 3) return false;
  if (data[1] == 0x0e) {
    opcode = static_cast<uint16_t>(data[4]) |
             (static_cast<uint16_t>(data[5]) << 8);
    status = data[6];
    return true;
  }
  if (data[1] == 0x0f && data[2] == 4 && data[3] != 0) {
    status = data[3];
    opcode = static_cast<uint16_t>(data[5]) |
             (static_cast<uint16_t>(data[6]) << 8);
    return true;
  }
  return false;
}

StateMachine::StateMachine()
    : desiredName_{}, workingName_{}, command_{},
      status_{Step::Unavailable, RadioState::Unknown, Error::None, 0, 0,
              false, false, 0, 0},
      sentAt_(0), sendReadySince_(0), retrySince_(0),
      radioBeforeCommand_(RadioState::Unknown), errorBeforeCommand_(Error::None),
      started_(false),
      sendReadyClockStarted_(false) {}

void StateMachine::start(bool controllerAvailable) {
  if (started_) return;
  started_ = true;
  if (!controllerAvailable) {
    status_.lastError = Error::StartupFailed;
    ++status_.failures;
    return;
  }
  status_.step = Step::Idle;
}

void StateMachine::setDeviceName(const char* name) {
  if (!validName(name)) {
    desiredName_[0] = '\0';
    status_.lastError = Error::InvalidDeviceName;
    return;
  }
  memcpy(desiredName_, name, strlen(name) + 1);
}

bool StateMachine::nameChanged() const {
  return strcmp(desiredName_, workingName_) != 0;
}

void StateMachine::beginTransaction() {
  memcpy(workingName_, desiredName_, sizeof(workingName_));
  status_.step = Step::Reset;
  sendReadyClockStarted_ = false;
}

void StateMachine::fail(Error error, uint32_t now, uint8_t status) {
  // A completion timeout was counted when first observed, including when the
  // matching completion is eventually consumed after its deadline.
  if (!(error == Error::ResponseTimeout && status_.completionTimedOut))
    ++status_.failures;
  status_.lastError = error;
  status_.hciStatus = status;
  status_.step = Step::Backoff;
  status_.pendingOpcode = 0;
  status_.awaitingCompletion = false;
  status_.completionTimedOut = false;
  retrySince_ = now;
  sendReadyClockStarted_ = false;
}

void StateMachine::recordCompletionTimeout() {
  if (status_.completionTimedOut) return;
  status_.completionTimedOut = true;
  status_.lastError = Error::ResponseTimeout;
  ++status_.failures;
  if (status_.step == Step::Reset || status_.step == Step::Enable ||
      status_.step == Step::Disable) status_.radio = RadioState::Unknown;
  // Retain ownership of the outstanding opcode. There is no HCI transaction
  // ID: resending it could mistake a delayed reply for a new command's reply.
}

const Command* StateMachine::poll(uint32_t now, bool allowWork,
                                  bool sendAvailable) {
  if (status_.step == Step::Unavailable) return nullptr;
  if (status_.awaitingCompletion) {
    if (static_cast<uint32_t>(now - sentAt_) >= kCompletionTimeoutMs)
      recordCompletionTimeout();
    return nullptr;
  }
  if (!allowWork) {
    // Busy device work should not spend the send-readiness timeout budget.
    sendReadyClockStarted_ = false;
    return nullptr;
  }
  if (status_.step == Step::Backoff) {
    if (static_cast<uint32_t>(now - retrySince_) < kRetryDelayMs) return nullptr;
    // Reset also stops potentially active advertising after a failed disable.
    beginTransaction();
  } else if (status_.step == Step::Running) {
    if (!nameChanged()) return nullptr;
    status_.step = Step::Disable;
    sendReadyClockStarted_ = false;
  } else if (status_.step == Step::Idle) {
    if (desiredName_[0] == '\0') return nullptr;
    beginTransaction();
  }

  // A queued name change between acknowledgments must not enable stale data.
  if (nameChanged() && (status_.step == Step::Parameters ||
      status_.step == Step::AdvertisingData ||
      status_.step == Step::ScanResponse || status_.step == Step::Enable)) {
    if (desiredName_[0] == '\0') {
      status_.step = Step::Idle;
      return nullptr;
    }
    beginTransaction();
  }

  if (!sendAvailable) {
    if (!sendReadyClockStarted_) {
      sendReadyClockStarted_ = true;
      sendReadySince_ = now;
    } else if (static_cast<uint32_t>(now - sendReadySince_) >= kSendReadyTimeoutMs) {
      fail(Error::SendUnavailable, now);
    }
    return nullptr;
  }

  buildCommand();
  status_.pendingOpcode = command_.opcode;
  status_.awaitingCompletion = true;
  status_.completionTimedOut = false;
  sentAt_ = now;
  radioBeforeCommand_ = status_.radio;
  errorBeforeCommand_ = status_.lastError;
  sendReadyClockStarted_ = false;
  ++status_.sentCommands;
  return &command_;
}

void StateMachine::complete(uint16_t opcode, uint8_t status, uint32_t receivedAt) {
  if (!status_.awaitingCompletion || opcode != status_.pendingOpcode) return;
  const bool receivedLate =
      static_cast<uint32_t>(receivedAt - sentAt_) >= kCompletionTimeoutMs;
  if (!receivedLate && status_.completionTimedOut) {
    // The callback can receive a timely event on another core just before the
    // loop checks the deadline, then publish it just after that check. Reception
    // time is authoritative; cancel that provisional timeout when it arrives.
    status_.completionTimedOut = false;
    --status_.failures;
    status_.radio = radioBeforeCommand_;
    status_.lastError = errorBeforeCommand_;
  }
  if (receivedLate)
    recordCompletionTimeout();

  if (status == 0) {
    if (status_.step == Step::Reset || status_.step == Step::Disable)
      status_.radio = RadioState::Off;
    else if (status_.step == Step::Enable)
      status_.radio = RadioState::On;
  }
  if (status_.completionTimedOut) {
    fail(Error::ResponseTimeout, receivedAt, status);
    return;
  }
  if (status != 0) {
    fail(Error::CommandRejected, receivedAt, status);
    return;
  }
  status_.awaitingCompletion = false;
  status_.pendingOpcode = 0;
  status_.hciStatus = 0;
  switch (status_.step) {
    case Step::Reset:
      status_.step = workingName_[0] == '\0' ? Step::Idle : Step::Parameters;
      break;
    case Step::Parameters: status_.step = Step::AdvertisingData; break;
    case Step::AdvertisingData: status_.step = Step::ScanResponse; break;
    case Step::ScanResponse: status_.step = Step::Enable; break;
    case Step::Enable:
      status_.step = Step::Running;
      status_.lastError = Error::None;
      break;
    case Step::Disable:
      status_.step = Step::Idle;
      workingName_[0] = '\0';
      break;
    default: break;
  }
}

void StateMachine::buildCommand() {
  memset(&command_, 0, sizeof(command_));
  command_.data[0] = 0x01;
  switch (status_.step) {
    case Step::Reset:
      command_.opcode = kReset;
      command_.size = 4;
      break;
    case Step::Parameters: {
      command_.opcode = kParameters;
      command_.size = 19;
      command_.data[4] = kIntervalUnits & 0xff;
      command_.data[5] = kIntervalUnits >> 8;
      command_.data[6] = kIntervalUnits & 0xff;
      command_.data[7] = kIntervalUnits >> 8;
      command_.data[8] = 0x02;  // ADV_SCAN_IND, scannable and non-connectable.
      command_.data[17] = 0x07; // All three advertising channels.
      break;
    }
    case Step::AdvertisingData:
    case Step::ScanResponse: {
      const bool scanResponse = status_.step == Step::ScanResponse;
      command_.opcode = scanResponse ? kScanResponse : kAdvertisingData;
      command_.size = 36;
      uint8_t* payload = command_.data + 5;
      uint8_t offset = 0;
      if (!scanResponse) {
        payload[offset++] = 2;
        payload[offset++] = 0x01;
        payload[offset++] = 0x06;
      }
      const uint8_t nameLength = static_cast<uint8_t>(strlen(workingName_));
      payload[offset++] = nameLength + 6;
      payload[offset++] = 0x09;
      memcpy(payload + offset, "HAS3:", 5);
      offset += 5;
      memcpy(payload + offset, workingName_, nameLength);
      offset += nameLength;
      payload[offset++] = 2;
      payload[offset++] = 0x0a;
      payload[offset++] = 3;  // Preserve the deployed receiver's advertised field.
      command_.data[4] = offset;
      break;
    }
    case Step::Enable:
    case Step::Disable:
      command_.opcode = kEnable;
      command_.size = 5;
      command_.data[4] = status_.step == Step::Enable ? 1 : 0;
      break;
    default: break;
  }
  command_.data[1] = command_.opcode & 0xff;
  command_.data[2] = command_.opcode >> 8;
  command_.data[3] = command_.size - 4;
}

Diagnostics StateMachine::diagnostics() const { return status_; }

}  // namespace Has1BleBeacon
