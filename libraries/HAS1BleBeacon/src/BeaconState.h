#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Has1BleBeacon {

constexpr size_t kMaxDeviceName = 18;
constexpr uint32_t kSendReadyTimeoutMs = 50;
constexpr uint32_t kCompletionTimeoutMs = 500;
constexpr uint32_t kRetryDelayMs = 2000;

enum class Step : uint8_t {
  Unavailable, Idle, Reset, Parameters, AdvertisingData, ScanResponse,
  Enable, Running, Disable, Backoff
};
enum class RadioState : uint8_t { Unknown, Off, On };
enum class Error : uint8_t {
  None, StartupFailed, InvalidDeviceName, SendUnavailable,
  CommandRejected, ResponseTimeout
};

struct Command {
  uint8_t data[36];
  uint8_t size;
  uint16_t opcode;
};

struct Diagnostics {
  Step step;
  RadioState radio;
  Error lastError;
  uint8_t hciStatus;
  uint16_t pendingOpcode;
  bool awaitingCompletion;
  bool completionTimedOut;
  uint32_t sentCommands;
  uint32_t failures;
};

// Decodes a final H4 Command Complete, or a rejecting Command Status.
// Successful Command Status is only acceptance, not completion, and is ignored.
bool decodeCommandEvent(const uint8_t* data, size_t size,
                        uint16_t& opcode, uint8_t& status);

// Portable, single-owner state machine. No controller calls, waits or allocation.
class StateMachine {
 public:
  StateMachine();
  // Called once; retains a name supplied before controller startup.
  void start(bool controllerAvailable);
  // Invalid/null/empty names request a stop. Copies at most 19 input bytes.
  void setDeviceName(const char* name);
  // Returns at most one command, already marked outstanding before return.
  // The returned buffer remains valid until the next poll that returns a command.
  // allowWork=false still checks timeout bookkeeping but never emits a command.
  const Command* poll(uint32_t now, bool allowWork, bool sendAvailable);
  // receivedAt is callback reception time, not the time the loop consumes it.
  void complete(uint16_t opcode, uint8_t status, uint32_t receivedAt);
  Diagnostics diagnostics() const;

 private:
  void beginTransaction();
  void fail(Error error, uint32_t now, uint8_t status = 0);
  void recordCompletionTimeout();
  void buildCommand();
  bool nameChanged() const;

  char desiredName_[kMaxDeviceName + 1];
  char workingName_[kMaxDeviceName + 1];
  Command command_;
  Diagnostics status_;
  uint32_t sentAt_;
  uint32_t sendReadySince_;
  uint32_t retrySince_;
  RadioState radioBeforeCommand_;
  Error errorBeforeCommand_;
  bool started_;
  bool sendReadyClockStarted_;
};

}  // namespace Has1BleBeacon
