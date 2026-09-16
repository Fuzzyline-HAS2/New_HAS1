#pragma once

#include "IoTGloveProtocol.h"

namespace iotglove {
namespace diagnostics {

enum class Event : uint8_t { Boot, Reset, Ota };
enum class Code : uint8_t {
  Reason, Requested, WifiStart, WifiOk, WifiFail, Checking, Flashing,
  Updated, Skipped, Failed, NvsFail, TaskFail, Disabled
};

struct Log {
  uint32_t sequence = 0;
  uint32_t bootId = 0;
  uint32_t uptimeMs = 0;
  Event event = Event::Boot;
  Code code = Code::Reason;
  uint32_t value = 0;
};

inline const char* eventName(Event event) {
  switch (event) {
    case Event::Boot: return "boot";
    case Event::Reset: return "reset";
    case Event::Ota: return "ota";
  }
  return nullptr;
}

inline const char* codeName(Code code) {
  switch (code) {
    case Code::Reason: return "reason";
    case Code::Requested: return "requested";
    case Code::WifiStart: return "wifi_start";
    case Code::WifiOk: return "wifi_ok";
    case Code::WifiFail: return "wifi_fail";
    case Code::Checking: return "checking";
    case Code::Flashing: return "flashing";
    case Code::Updated: return "updated";
    case Code::Skipped: return "skipped";
    case Code::Failed: return "failed";
    case Code::NvsFail: return "nvs_fail";
    case Code::TaskFail: return "task_fail";
    case Code::Disabled: return "disabled";
  }
  return nullptr;
}

inline bool validLog(const Log& log) {
  if (!log.sequence || !eventName(log.event) || !codeName(log.code)) return false;
  if (log.event == Event::Boot)
    return log.sequence == 1 && log.uptimeMs == 0 && log.code == Code::Reason;
  if (log.sequence < 2) return false;
  if (log.event == Event::Reset) return log.code == Code::Requested && log.value == 1;
  return log.event == Event::Ota && log.code >= Code::WifiStart &&
      log.code <= Code::Disabled && log.value != 0;
}

inline bool putNumber(wire::Frame& frame, uint32_t value) {
  char text[16];
  snprintf(text, sizeof(text), "%lu", static_cast<unsigned long>(value));
  return wire::put(frame, text);
}

inline bool makeLogFrame(const Log& log, wire::Frame& frame) {
  if (!validLog(log)) return false;
  wire::Frame next;
  strcpy(next.type, "LOG");
  next.id = log.sequence;
  if (!putNumber(next, log.bootId) || !putNumber(next, log.uptimeMs) ||
      !wire::put(next, eventName(log.event)) || !wire::put(next, codeName(log.code)) ||
      !putNumber(next, log.value)) return false;
  frame = next;
  return true;
}

inline bool parseLog(const wire::Frame& frame, Log& log) {
  if (strcmp(frame.type, "LOG") || frame.count != 5) return false;
  Log next;
  next.sequence = frame.id;
  if (!wire::uint32(frame.args[0], next.bootId) ||
      !wire::uint32(frame.args[1], next.uptimeMs) ||
      !wire::uint32(frame.args[4], next.value)) return false;
  bool foundEvent = false, foundCode = false;
  for (uint8_t i = 0; i <= static_cast<uint8_t>(Event::Ota); ++i) {
    const auto event = static_cast<Event>(i);
    if (!strcmp(frame.args[2], eventName(event))) { next.event = event; foundEvent = true; break; }
  }
  for (uint8_t i = 0; i <= static_cast<uint8_t>(Code::Disabled); ++i) {
    const auto code = static_cast<Code>(i);
    if (!strcmp(frame.args[3], codeName(code))) { next.code = code; foundCode = true; break; }
  }
  if (!foundEvent || !foundCode || !validLog(next)) return false;
  log = next;
  return true;
}

// The Beetle uses this exact queue inside a short portMUX critical section.
// Neither queue method blocks or emits I/O. Full queues drop the newest log;
// protocol commands, heartbeat, and OTA results use their existing paths.
class LogQueue {
 public:
  static constexpr size_t kCapacity = 8;
  static constexpr size_t kDrainBudget = 2;

  bool push(const Log& log) {
    if (count_ == kCapacity) { if (dropped_ != UINT32_MAX) ++dropped_; return false; }
    entries_[(head_ + count_) % kCapacity] = log;
    ++count_;
    return true;
  }
  bool pop(Log& log) {
    if (!count_) return false;
    log = entries_[head_];
    head_ = (head_ + 1) % kCapacity;
    --count_;
    return true;
  }
  size_t size() const { return count_; }
  uint32_t dropped() const { return dropped_; }
 private:
  Log entries_[kCapacity];
  size_t head_ = 0, count_ = 0;
  uint32_t dropped_ = 0;
};

}  // namespace diagnostics
}  // namespace iotglove
