#pragma once

#include <stddef.h>
#include <stdint.h>
#include <IoTGloveDiagnostics.h>

namespace iotglove {

// BOOT is replayed on every PING with sequence 1. Cache it separately so a
// late snapshot is accepted even after newer OTA events from this boot.
class BeetleLogTracker {
 public:
  bool accept(const diagnostics::Log& log, bool peerKnown, uint32_t peerBoot) {
    if (!peerKnown || log.bootId != peerBoot) return false;
    if (!known_ || boot_ != peerBoot) {
      known_ = true; boot_ = peerBoot; sequence_ = 0; reasonKnown_ = false;
    }
    if (log.event == diagnostics::Event::Boot) {
      if (reasonKnown_) return false;
      reasonKnown_ = true; reason_ = log.value; return true;
    }
    if (static_cast<int32_t>(log.sequence - sequence_) <= 0) return false;
    sequence_ = log.sequence; return true;
  }
  bool reasonKnown(uint32_t peerBoot) const { return known_ && boot_ == peerBoot && reasonKnown_; }
  uint32_t reason() const { return reason_; }
 private:
  bool known_ = false, reasonKnown_ = false;
  uint32_t boot_ = 0, sequence_ = 0, reason_ = 0;
};

// Callers provide synchronization. Retain the newest bounded log bytes while a
// console is absent/slow; never wait for a producer or allocate per log message.
template <size_t Capacity>
class ConsoleByteRing {
 public:
  void write(const uint8_t* data, size_t size) {
    for (size_t n = 0; n < size; ++n) {
      if (size_ == Capacity) { head_ = (head_ + 1) % Capacity; --size_; ++dropped_; }
      bytes_[(head_ + size_) % Capacity] = data[n];
      ++size_;
    }
  }
  size_t read(uint8_t* out, size_t capacity) {
    const size_t count = size_ < capacity ? size_ : capacity;
    for (size_t n = 0; n < count; ++n) out[n] = bytes_[(head_ + n) % Capacity];
    head_ = (head_ + count) % Capacity; size_ -= count;
    return count;
  }
  size_t size() const { return size_; }
  uint32_t dropped() const { return dropped_; }
 private:
  uint8_t bytes_[Capacity] = {};
  size_t head_ = 0, size_ = 0;
  uint32_t dropped_ = 0;
};

// Telnet commands are exactly one approved character followed by Enter. Words
// such as "status" must not accidentally execute their embedded 'u' OTA command.
class TelnetCommandParser {
 public:
  bool feed(uint8_t byte, char& command) {
    if (state_ == State::Iac) {
      if (byte >= 251 && byte <= 254) state_ = State::Option;
      else if (byte == 250) state_ = State::Subnegotiation;
      else { state_ = State::Data; if (byte == 255) invalid_ = true; }
      return false;
    }
    if (state_ == State::Option) { state_ = State::Data; return false; }
    if (state_ == State::Subnegotiation) { if (byte == 255) state_ = State::SubIac; return false; }
    if (state_ == State::SubIac) {
      state_ = byte == 240 ? State::Data : State::Subnegotiation;
      return false;
    }
    if (byte == 255) { state_ = State::Iac; return false; }
    if (byte == '\r' || byte == '\n') {
      const bool ready = !invalid_ && pending_ && allowed(pending_);
      command = pending_; pending_ = 0; invalid_ = false;
      return ready;
    }
    if ((byte == 8 || byte == 127) && !invalid_) { pending_ = 0; return false; }
    if (byte < 32 || byte > 126 || pending_) invalid_ = true;
    else pending_ = static_cast<char>(byte);
    return false;
  }
  void reset() { state_ = State::Data; pending_ = 0; invalid_ = false; }
 private:
  enum class State : uint8_t { Data, Iac, Option, Subnegotiation, SubIac };
  static bool allowed(char c) { return c == 's' || c == '?' || c == 'p' || c == 'b' || c == 'u'; }
  State state_ = State::Data;
  char pending_ = 0;
  bool invalid_ = false;
};

class ConsoleCommandBudget {
 public:
  bool allow(uint32_t now) {
    if (uint32_t(now - windowAt_) >= 1000U) { windowAt_ = now; count_ = 0; }
    if (count_ == 8) return false;
    ++count_; return true;
  }
 private:
  uint32_t windowAt_ = 0;
  uint8_t count_ = 0;
};

// Parse only known library lifecycle markers and return fixed safe text. Raw
// SSIDs, URLs, HTTP bodies and overlong lines never reach either console.
class WifiLifecycleFilter {
 public:
  const char* feed(uint8_t byte) {
    if (byte == '\n') {
      line_[length_] = '\0';
      const char* result = overflow_ ? nullptr : classify();
      length_ = 0; overflow_ = false;
      return result;
    }
    if (byte != '\r') {
      if (length_ + 1 < sizeof(line_)) line_[length_++] = static_cast<char>(byte);
      else overflow_ = true;
    }
    return nullptr;
  }
 private:
  const char* classify() const {
    if (!strncmp(line_, "Try WiFi:", 9) || !strncmp(line_, "Try saved WiFi:", 15))
      return "[wifi] attempting configured network\n";
    if (!strcmp(line_, "WiFi connected")) return "[wifi] connected\n";
    if (!strcmp(line_, "WiFi connect failed")) return "[wifi] connection attempt failed\n";
    if (!strcmp(line_, "WiFi scan failed")) return "[wifi] scan failed\n";
    if (!strcmp(line_, "WiFi disconnected. Reconnecting...")) return "[wifi] reconnecting\n";
    return nullptr;
  }
  char line_[160] = {};
  size_t length_ = 0;
  bool overflow_ = false;
};

}  // namespace iotglove
