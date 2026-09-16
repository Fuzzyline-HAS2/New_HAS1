#pragma once

#include "peer_state.h"
#include <stdarg.h>

namespace iotglove {

enum class ProbeState : uint8_t { Never, Waiting, Matched, Timeout, SendFailed };
enum class ResetObservation : uint8_t { Never, Waiting, RebootObserved, BaselineUnknown, Timeout };

// No UART protocol changes: only a valid HELLO echoing the exact current PING
// request ID establishes two-way communication. HEART and unsolicited HELLO do not.
class LinkDiagnostics {
 public:
  static constexpr uint32_t kProbeTimeoutMs = 1500;
  static constexpr uint32_t kResetTimeoutMs = 15000;
  bool beginProbe(uint32_t id, uint32_t now) {
    tick(now);
    if (!id || probe_ == ProbeState::Waiting) return false;
    probe_ = ProbeState::Waiting;
    probeId_ = id; probeSentAt_ = now;
    probeResetEpoch_ = reset_ == ResetObservation::Waiting ? resetEpoch_ : 0;
    ++probesSent_;
    return true;
  }
  void probeSendFailed() { if (probe_ == ProbeState::Waiting) probe_ = ProbeState::SendFailed; }
  bool observeHello(uint32_t responseId, const PeerState& peer, uint32_t now) {
    tick(now);
    if (probe_ != ProbeState::Waiting || responseId != probeId_ || !peer.known()) return false;
    probe_ = ProbeState::Matched;
    probeRtt_ = uint32_t(now - probeSentAt_);
    matchedAt_ = now; matchedBoot_ = peer.bootId();
    ++probesMatched_;
    // Baseline is captured at the actual HIGH pulse. A PING issued beforehand,
    // or a new boot observed only in an unsolicited HELLO, cannot confirm reset.
    if (reset_ == ResetObservation::Waiting && probeResetEpoch_ == resetEpoch_) {
      if (!resetBaselineKnown_) reset_ = ResetObservation::BaselineUnknown;
      else if (peer.bootId() != resetBaselineBoot_) reset_ = ResetObservation::RebootObserved;
      resetObservedBoot_ = peer.bootId();
    }
    return true;
  }
  void beginReset(const PeerState& peer, uint32_t now) {
    if (++resetEpoch_ == 0) ++resetEpoch_;
    reset_ = ResetObservation::Waiting;
    resetStartedAt_ = now;
    resetBaselineKnown_ = peer.helloFresh(now);
    resetBaselineBoot_ = resetBaselineKnown_ ? peer.bootId() : 0;
    resetObservedBoot_ = 0;
  }
  void tick(uint32_t now) {
    if (probe_ == ProbeState::Waiting && uint32_t(now - probeSentAt_) >= kProbeTimeoutMs) {
      probe_ = ProbeState::Timeout; ++probeTimeouts_;
    }
    if (reset_ == ResetObservation::Waiting && uint32_t(now - resetStartedAt_) >= kResetTimeoutMs)
      reset_ = ResetObservation::Timeout;
  }
  ProbeState probeState() const { return probe_; }
  uint32_t probeId() const { return probeId_; }
  uint32_t probeAge(uint32_t now) const { return uint32_t(now - probeSentAt_); }
  uint32_t probeRtt() const { return probeRtt_; }
  bool matchedFresh(const PeerState& peer, uint32_t now) const {
    return probe_ == ProbeState::Matched && peer.online(now) && matchedBoot_ == peer.bootId() &&
        uint32_t(now - matchedAt_) < kLocationFreshMs;
  }
  uint32_t probesSent() const { return probesSent_; }
  uint32_t probesMatched() const { return probesMatched_; }
  uint32_t probeTimeouts() const { return probeTimeouts_; }
  ResetObservation resetState() const { return reset_; }
  bool resetBaselineKnown() const { return resetBaselineKnown_; }
  uint32_t resetBaselineBoot() const { return resetBaselineBoot_; }
  uint32_t resetObservedBoot() const { return resetObservedBoot_; }
  uint32_t resetAge(uint32_t now) const { return uint32_t(now - resetStartedAt_); }
 private:
  ProbeState probe_ = ProbeState::Never;
  ResetObservation reset_ = ResetObservation::Never;
  uint32_t probeId_ = 0, probeSentAt_ = 0, probeRtt_ = 0, matchedAt_ = 0, matchedBoot_ = 0;
  uint32_t probesSent_ = 0, probesMatched_ = 0, probeTimeouts_ = 0;
  uint32_t resetEpoch_ = 0, probeResetEpoch_ = 0, resetStartedAt_ = 0;
  bool resetBaselineKnown_ = false;
  uint32_t resetBaselineBoot_ = 0, resetObservedBoot_ = 0;
};

// Fixed queue for USB diagnostics: the application drains only available TX
// capacity in small pieces. A disconnected/full console cannot stall inputs.
class DiagnosticLog {
 public:
  bool append(const char* format, ...) {
    if (offset_ == length_) offset_ = length_ = 0;
    va_list args;
    va_start(args, format);
    const int count = vsnprintf(buffer_ + length_, sizeof(buffer_) - length_, format, args);
    va_end(args);
    if (count < 0 || static_cast<size_t>(count) >= sizeof(buffer_) - length_) {
      buffer_[length_] = '\0';
      ++dropped_;
      return false;
    }
    length_ += static_cast<size_t>(count);
    return true;
  }
  const char* data() const { return buffer_ + offset_; }
  size_t pending() const { return length_ - offset_; }
  void consumed(size_t count) {
    if (count > pending()) count = pending();
    offset_ += count;
    if (offset_ == length_) offset_ = length_ = 0;
  }
  uint32_t dropped() const { return dropped_; }
 private:
  char buffer_[1536] = {};
  size_t offset_ = 0, length_ = 0;
  uint32_t dropped_ = 0;
};

}  // namespace iotglove
