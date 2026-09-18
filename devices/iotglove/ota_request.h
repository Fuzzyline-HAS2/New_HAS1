#pragma once

#include "game_state.h"
#include "library_and_pin.h"
#include <string.h>

namespace iotglove {

inline bool otaAllowedPhase(Phase phase) {
  return phase == Phase::Setting || phase == Phase::Ready || phase == Phase::Ended;
}

struct BoardUpdateRequest {
  uint32_t ttgoVersion = 0;
  uint32_t beetleVersion = 0;
  char sourceCommand[40] = {};
};

// The application loop owns this coordinator. Observing a new server command
// may replace a waiting request, but can never rewrite a running board pair.
class OtaRequests {
 public:
  // Returns true once for each distinct malformed OTA command, for diagnostics.
  bool observe(const ServerSnapshot& snapshot) {
    if (!snapshot.valid) return false;  // A reconnect is not a new OTA command.
    const char* command = snapshot.updateCommand;
    if (strcmp(command, observed_) == 0) return false;
    strcpy(observed_, command);
    if (!snapshot.updateRequested) {
      if (hasPending_ && pending_.sourceCommand[0]) hasPending_ = false;
      return command[0] != '\0';
    }
    if (hasActive_ && strcmp(command, active_.sourceCommand) == 0) {
      if (hasPending_ && pending_.sourceCommand[0]) hasPending_ = false;
      return false;
    }
    pending_ = BoardUpdateRequest{};
    pending_.ttgoVersion = snapshot.otaTtgoVersion;
    pending_.beetleVersion = snapshot.otaBeetleVersion;
    strcpy(pending_.sourceCommand, command);
    hasPending_ = true;
    return false;
  }
  void requestLatest() { pending_ = BoardUpdateRequest{}; hasPending_ = true; }
  bool hasPending() const { return hasPending_; }
  bool hasActive() const { return hasActive_; }
  const BoardUpdateRequest& pending() const { return pending_; }
  const BoardUpdateRequest& active() const { return active_; }
  bool startPending(uint32_t now = 0) {
    if (!hasPending_ || hasActive_) return false;
    active_ = pending_;
    hasActive_ = true;
    hasPending_ = false;
    activeStarted_ = now;
    return true;
  }
  // A completed Beetle must not leave the application locked throughout a game
  // or an indefinite server outage. Never interrupt a still-running Beetle or
  // an already-submitted TTGO flash; those workers own their completion/timeout.
  bool abortReadyPair(uint32_t now, bool peerReady, bool ttgoSubmitted,
                      bool serverFresh, Phase phase) {
    if (!hasActive_ || !peerReady || ttgoSubmitted) return false;
    if ((serverFresh && !otaAllowedPhase(phase)) ||
        uint32_t(now - activeStarted_) >= kBeetleOtaTimeoutMs) {
      finishActive();
      return true;
    }
    return false;
  }
  void finishActive() { hasActive_ = false; }
 private:
  BoardUpdateRequest pending_, active_;
  char observed_[40] = {};
  uint32_t activeStarted_ = 0;
  bool hasPending_ = false, hasActive_ = false;
};

}  // namespace iotglove
