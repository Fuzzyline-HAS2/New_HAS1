#include "game_model.h"
#include "state_policy.h"

#include <string.h>

namespace iotglove {

void DebouncedInput::begin(bool level, uint32_t now) {
  stable_ = candidate_ = level;
  candidateSince_ = now;
}

bool DebouncedInput::update(bool level, uint32_t now) {
  if (level != candidate_) {
    candidate_ = level;
    candidateSince_ = now;
  }
  if (stable_ == candidate_ || uint32_t(now - candidateSince_) < intervalMs_) return false;
  stable_ = candidate_;
  return true;
}

void GameModel::resetQueue() {
  head_ = size_ = 0;
  rolePending_ = countPending_ = false;
}

void GameModel::begin(bool chipPresent, uint32_t now) {
  chipPresent_ = chipPresent;
  trainingGhost_ = !chipPresent;  // A missing chip at boot must not show green.
  trainingStart_ = stepStart_ = now;
  count_ = 0;
  haveServer_ = needsSync_ = overflow_ = false;
  insertReported_ = false;
  haptic_ = Haptic::None;
  resetQueue();
}

bool GameModel::emit(GameEvent::Kind kind, uint8_t value) {
  if (needsSync_ || !haveServer_) return false;
  if (size_ == kCapacity) {
    // Stop producing mutations; keep existing ordered events for diagnostics.
    overflow_ = needsSync_ = true;
    return false;
  }
  GameEvent& event = queue_[(head_ + size_) % kCapacity];
  event = GameEvent{};
  event.kind = kind;
  event.value = value;
  event.sequence = ++sequence_;
  memcpy(event.session, server_.session, sizeof(event.session));
  memcpy(event.deviceName, server_.deviceName, sizeof(event.deviceName));
  ++size_;
  return true;
}

bool GameModel::peekEvent(GameEvent& out) const {
  if (!size_ || needsSync_) return false;
  out = queue_[head_];
  return true;
}

void GameModel::consumeEvent() {
  if (size_) { head_ = (head_ + 1) % kCapacity; --size_; }
}

void GameModel::commandUncertain(uint32_t /* sequence */) {
  // The network adapter follows this with a fresh authoritative snapshot.
  // Never retry life_chip deltas or role=ghost (which clears server flags).
  needsSync_ = true;
  resetQueue();
}

bool GameModel::activeGhost() const {
  return haveServer_ && gameMutationsAllowed(server_) && server_.role == Role::Ghost;
}

void GameModel::applyServer(const ServerSnapshot& snapshot, uint32_t now) {
  if (profile_ == Profile::Training) return;
  if (!snapshot.valid) {
    needsSync_ = true;
    server_.valid = false;
    haptic_ = Haptic::None;
    resetQueue();
    return;
  }
  const bool newSession = !haveServer_ ||
      strcmp(snapshot.session, server_.session) != 0 ||
      strcmp(snapshot.deviceName, server_.deviceName) != 0;
  const bool phaseChanged = haveServer_ && snapshot.phase != server_.phase;
  const bool roleChanged = haveServer_ && snapshot.role != server_.role;
  const bool controlChanged = haveServer_ &&
      gameMutationsAllowed(snapshot) != gameMutationsAllowed(server_);
  const bool recovering = needsSync_;
  const bool playerAcknowledged = rolePending_ && pendingRole_ == Role::Player &&
      snapshot.role == Role::Player;
  if (newSession || phaseChanged || controlChanged || recovering) {
    resetQueue();
    haptic_ = Haptic::None;
    insertReported_ = false;
    needsSync_ = overflow_ = false;
  }
  if (rolePending_ && snapshot.role == pendingRole_) rolePending_ = false;
  const uint32_t nextInterval = snapshot.stepSeconds <= 86400U ? snapshot.stepSeconds * 1000U : 0;
  const uint8_t receivedCount = snapshot.revivalCount > 4 ? 4 : snapshot.revivalCount;
  // Repeated snapshots do not reset a partly charged step. Remote changes and
  // interval changes deliberately restart only the current step, not the count.
  const bool countAcknowledged = countPending_ && receivedCount == count_;
  if (newSession || phaseChanged || controlChanged || roleChanged || recovering ||
      nextInterval != intervalMs_ || (receivedCount != observedCount_ && !countPending_)) {
    count_ = receivedCount;
    countPending_ = false;
    stepStart_ = now;
  } else if (countAcknowledged) {
    countPending_ = false;
  }
  observedCount_ = receivedCount;
  intervalMs_ = nextInterval;
  server_ = snapshot;
  haveServer_ = true;
  // A reboot does not fabricate an insertion delta. A stored chip count of one
  // plus a present physical chip does allow completing an already-started return.
  if (newSession || recovering)
    insertReported_ = chipPresent_ && snapshot.lifeChip == 1;
  if (!newSession && !phaseChanged && !controlChanged && !recovering && playerAcknowledged &&
      !chipPresent_ && gameMutationsAllowed(server_) && server_.capturesAllowed &&
      emit(GameEvent::Kind::Capture)) {
    // A removal during revival acknowledgement belongs to the next capture.
    // It was not also reported as a ghost delta, which would count it twice.
    rolePending_ = true;
    pendingRole_ = Role::Ghost;
    insertReported_ = false;
    haptic_ = Haptic::Removed;
  }
  // A physical insertion during the capture round trip is processed only once
  // its ghost role is confirmed. No fabricated boot/reconnect chip delta.
  if (!newSession && !phaseChanged && !controlChanged && !recovering && roleChanged &&
      server_.role == Role::Ghost && chipPresent_ && insertReported_) attemptReturn();
}

void GameModel::chipChanged(bool present, uint32_t now) {
  if (chipPresent_ == present) return;
  chipPresent_ = present;
  if (profile_ == Profile::Training) {
    if (!present) {
      trainingGhost_ = true;
      trainingStart_ = now;
      haptic_ = Haptic::Removed;
    }
    tick(now);
    return;
  }
  if (!synchronized() || !gameMutationsAllowed(server_)) return;
  if (rolePending_ && pendingRole_ == Role::Player) return;
  if (server_.role == Role::Player && !rolePending_) {
    if (!present && server_.capturesAllowed && emit(GameEvent::Kind::Capture)) {
      rolePending_ = true;
      pendingRole_ = Role::Ghost;
      insertReported_ = false;
      haptic_ = Haptic::Removed;
    }
    return;
  }
  // Capture acknowledgement may arrive after a quick remove/reinsert cycle.
  if (server_.role == Role::Ghost || (rolePending_ && pendingRole_ == Role::Ghost)) {
    if (emit(present ? GameEvent::Kind::ChipInserted : GameEvent::Kind::ChipRemoved)) {
      insertReported_ = present;
      if (present) attemptReturn();
    }
  }
}

void GameModel::attemptReturn() {
  if (!activeGhost() || !chipPresent_ || !insertReported_ || rolePending_ || needsSync_) return;
  const bool pendingCapture = !server_.sacrificed && !server_.open;
  const bool eligibleRevival = server_.sacrificed && server_.open && count_ == 4;
  if ((pendingCapture || eligibleRevival) &&
      emit(pendingCapture ? GameEvent::Kind::CancelCapture : GameEvent::Kind::Revive)) {
    rolePending_ = true;
    pendingRole_ = Role::Player;
  }
}

void GameModel::buttonPressed(uint32_t now) {
  if (profile_ == Profile::Training) {
    if (!trainingGhost_) return;
    trainingStart_ = now;
    haptic_ = Haptic::Found;
    return;
  }
  if (!synchronized() || !activeGhost() || rolePending_) return;
  if (emit(GameEvent::Kind::SetCount, 0)) {
    count_ = 0;
    countPending_ = true;
    stepStart_ = now;
    haptic_ = Haptic::Found;
  }
}

void GameModel::tick(uint32_t now) {
  if (profile_ == Profile::Training) {
    if (trainingGhost_ && chipPresent_ && uint32_t(now - trainingStart_) >= 9000U)
      trainingGhost_ = false;
    count_ = trainingGhost_ ? uint8_t(1 + (uint32_t(now - trainingStart_) >= 9000U ? 3 :
        uint32_t(now - trainingStart_) / 3000U)) : 4;
    return;
  }
  if (!synchronized() || !activeGhost() || rolePending_ || !intervalMs_) return;
  // Conservative pending-capture policy: start counting after capture, but cap
  // at 3 until altar confirmation. No life-device permission before sacrifice.
  const uint8_t cap = server_.sacrificed ? 4 : 3;
  if (count_ < cap && uint32_t(now - stepStart_) >= intervalMs_) {
    const uint32_t steps = uint32_t(now - stepStart_) / intervalMs_;
    const uint8_t next = steps >= uint32_t(cap - count_) ? cap : uint8_t(count_ + steps);
    if (emit(GameEvent::Kind::SetCount, next)) {
      count_ = next;
      countPending_ = true;
      stepStart_ += steps * intervalMs_;
    }
  }
  attemptReturn();
}

bool GameModel::canUseLifeDevice() const {
  return synchronized() && activeGhost() && server_.sacrificed && !server_.open && count_ == 4;
}

Feedback GameModel::feedback() {
  Feedback out;
  out.haptic = haptic_;
  haptic_ = Haptic::None;
  if (profile_ == Profile::Training) {
    out.stateValid = true;  // Fixed default identity preserves explicit training events.
    out.display = trainingGhost_ ? Display::Ghost : Display::Player;
    out.lit = trainingGhost_ ? count_ : 4;
    return out;
  }
  out.stateValid = synchronized() && server_.valid;
  out.role = server_.role;
  out.deviceState = server_.deviceState;
  out.phase = server_.phase;
  out.stateEpoch = server_.connectionEpoch;
  if (!haveServer_) return out;
  // Terminal/exploration and stale-state displays cannot be overridden by roles.
  if (server_.phase == Phase::Ended || server_.deviceState == DeviceState::Ended) { out.display = Display::Ended; return out; }
  if (needsSync_ || server_.phase == Phase::Exploration || server_.deviceState == DeviceState::Exploration) {
    out.display = server_.phase == Phase::Setting || server_.phase == Phase::Unknown ?
        Display::Setting : Display::Ready;
    return out;
  }
  if (server_.phase == Phase::Unknown) return out;
  if (server_.deviceState == DeviceState::Setting || server_.deviceState == DeviceState::Ready) {
    out.display = server_.deviceState == DeviceState::Setting ? Display::Setting : Display::Ready;
    out.lit = chipPresent_ ? 4 : 3;
    return out;
  }
  // The selection animation is meaningful before game_state becomes activate.
  if (server_.role == Role::Tagger &&
      (server_.deviceState == DeviceState::Blink || server_.deviceState == DeviceState::Activate)) {
    out.display = server_.deviceState == DeviceState::Blink ? Display::TaggerBlink : Display::TaggerActive;
    return out;
  }
  if (server_.phase == Phase::Setting) return out;
  if (server_.phase == Phase::Ready) { out.display = Display::Ready; return out; }
  switch (server_.role) {
    case Role::Player:
      out.display = (rolePending_ && pendingRole_ == Role::Ghost) ? Display::Ghost :
          (chipPresent_ ? Display::Player : Display::Ready);
      out.lit = out.display == Display::Ghost ? 0 : 4;
      break;
    case Role::Ghost: out.display = Display::Ghost; out.lit = count_; break;
    case Role::Tagger: out.display = Display::Tagger; break;
    default: out.display = Display::Ready; break;
  }
  return out;
}

}  // namespace iotglove
