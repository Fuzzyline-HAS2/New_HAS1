#pragma once

#include <stdint.h>
#include <string.h>
#include <TagMachineOtaProtocol.h>

namespace tagmachine {

inline bool freshReadyProof(bool rfidReady, uint32_t beforeCount,
                            uint32_t afterCount) {
  return rfidReady && afterCount != beforeCount;
}

enum class BeetleOtaState : uint8_t {
  Idle,
  Discovering,
  Capable,
  Waiting,
  InDoubt,
  Ready,
  Failed,
};

// Pure state policy kept independent from Arduino so request correlation,
// timeout behavior, and post-reboot proof can be regression-tested on a host.
class BeetleOtaPeer {
 public:
  void discover(uint32_t request, uint32_t now) {
    request_ = request;
    started_ = now;
    state_ = request ? BeetleOtaState::Discovering : BeetleOtaState::Failed;
    haveVersion_ = false;
    versionProofAfterBegin_ = false;
    updatedFirmware_ = 0;
    skippedBoot_ = 0;
    skipProofPending_ = false;
    progressSeen_ = false;
  }

  bool receiveVersion(const ota_wire::Response& response, uint32_t now) {
    if (response.type != ota_wire::ResponseType::Version || response.request != request_ ||
        response.protocol != ota_wire::kProtocolVersion) return false;
    firmware_ = response.firmware;
    partition_ = response.partition;
    boot_ = response.boot;
    lastSeen_ = now;
    haveVersion_ = true;
    if (state_ == BeetleOtaState::Discovering) {
      state_ = BeetleOtaState::Capable;
    } else if (state_ == BeetleOtaState::Waiting ||
               state_ == BeetleOtaState::InDoubt) {
      versionProofAfterBegin_ = true;
      confirmUpdated();
      confirmSkipped();
    }
    return true;
  }

  bool begin(uint32_t now, uint32_t requiredPartition,
             uint32_t targetVersion = 0) {
    if (state_ != BeetleOtaState::Capable || !haveVersion_ ||
        partition_ != requiredPartition ||
        (targetVersion && targetVersion < firmware_)) {
      state_ = BeetleOtaState::Failed;
      return false;
    }
    originalFirmware_ = firmware_;
    originalPartition_ = partition_;
    originalBoot_ = boot_;
    targetVersion_ = targetVersion;
    started_ = now;
    versionProofAfterBegin_ = false;
    updatedFirmware_ = 0;
    skippedBoot_ = 0;
    skipProofPending_ = false;
    progressSeen_ = false;
    state_ = BeetleOtaState::Waiting;
    return true;
  }

  bool receiveOutcome(const ota_wire::Response& response, uint32_t now) {
    if (response.type != ota_wire::ResponseType::Outcome || response.request != request_ ||
        (state_ != BeetleOtaState::Waiting &&
         state_ != BeetleOtaState::InDoubt)) return false;
    lastSeen_ = now;
    switch (response.outcome) {
      case ota_wire::Outcome::Accepted:
      case ota_wire::Outcome::Flashing:
        // Progress is not installation proof.
        progressSeen_ = true;
        state_ = BeetleOtaState::Waiting;
        return true;
      case ota_wire::Outcome::Skipped:
        progressSeen_ = true;
        if (response.firmware != originalFirmware_ ||
            response.partition != originalPartition_ ||
            (targetVersion_ && response.firmware != targetVersion_)) {
          state_ = BeetleOtaState::Failed;
          return true;
        }
        if (response.boot == originalBoot_) {
          state_ = BeetleOtaState::Ready;
          return true;
        }
        // A reset after persisting Skipped but before UART delivery is still a
        // valid no-op. Require a matching post-begin RV for the new boot before
        // accepting the replayed terminal result.
        skippedBoot_ = response.boot;
        skipProofPending_ = true;
        confirmSkipped();
        return true;
      case ota_wire::Outcome::Updated:
        progressSeen_ = true;
        if (response.firmware <= originalFirmware_ ||
            (targetVersion_ && response.firmware != targetVersion_) ||
            response.partition != originalPartition_ ||
            response.boot == originalBoot_) {
          state_ = BeetleOtaState::Failed;
          return true;
        }
        updatedFirmware_ = response.firmware;
        updatedBoot_ = response.boot;
        confirmUpdated();
        return true;
      case ota_wire::Outcome::Failed:
      case ota_wire::Outcome::Disabled:
        state_ = BeetleOtaState::Failed;
        return true;
      case ota_wire::Outcome::PeerBusy:
        state_ = BeetleOtaState::InDoubt;
        return true;
    }
    state_ = BeetleOtaState::Failed;
    return true;
  }

  void tick(uint32_t now, uint32_t discoveryTimeout, uint32_t updateTimeout) {
    if (state_ == BeetleOtaState::Discovering && discoveryTimeout &&
        static_cast<uint32_t>(now - started_) >= discoveryTimeout)
      state_ = BeetleOtaState::Failed;
    else if (state_ == BeetleOtaState::Waiting && updateTimeout &&
             static_cast<uint32_t>(now - started_) >= updateTimeout)
      state_ = BeetleOtaState::InDoubt;
  }

  BeetleOtaState state() const { return state_; }
  uint32_t request() const { return request_; }
  uint32_t firmware() const { return firmware_; }
  uint32_t partition() const { return partition_; }
  uint32_t boot() const { return boot_; }
  uint32_t lastSeen() const { return lastSeen_; }
  bool progressSeen() const { return progressSeen_; }

 private:
  void confirmUpdated() {
    if (updatedFirmware_ && versionProofAfterBegin_ &&
        firmware_ == updatedFirmware_ && boot_ == updatedBoot_ &&
        partition_ == originalPartition_) state_ = BeetleOtaState::Ready;
  }

  void confirmSkipped() {
    if (skipProofPending_ && versionProofAfterBegin_ &&
        firmware_ == originalFirmware_ && boot_ == skippedBoot_ &&
        partition_ == originalPartition_) state_ = BeetleOtaState::Ready;
  }

  BeetleOtaState state_ = BeetleOtaState::Idle;
  uint32_t request_ = 0;
  uint32_t started_ = 0;
  uint32_t lastSeen_ = 0;
  uint32_t firmware_ = 0;
  uint32_t partition_ = 0;
  uint32_t boot_ = 0;
  uint32_t originalFirmware_ = 0;
  uint32_t originalPartition_ = 0;
  uint32_t originalBoot_ = 0;
  uint32_t targetVersion_ = 0;
  uint32_t updatedFirmware_ = 0;
  uint32_t updatedBoot_ = 0;
  uint32_t skippedBoot_ = 0;
  bool haveVersion_ = false;
  bool versionProofAfterBegin_ = false;
  bool progressSeen_ = false;
  bool skipProofPending_ = false;
};

enum class BeetleOtaStage : uint8_t {
  Idle,
  DiscoverMain,
  UpdateMain,
  DiscoverSub,
  UpdateSub,
  ReadyForTtgo,
  Failed,
};

class BeetleOtaSequence {
 public:
  void start() { stage_ = BeetleOtaStage::DiscoverMain; }
  void discovered() {
    if (stage_ == BeetleOtaStage::DiscoverMain) stage_ = BeetleOtaStage::UpdateMain;
    else if (stage_ == BeetleOtaStage::DiscoverSub) stage_ = BeetleOtaStage::UpdateSub;
    else stage_ = BeetleOtaStage::Failed;
  }
  void completed() {
    if (stage_ == BeetleOtaStage::UpdateMain) stage_ = BeetleOtaStage::DiscoverSub;
    else if (stage_ == BeetleOtaStage::UpdateSub) stage_ = BeetleOtaStage::ReadyForTtgo;
    else stage_ = BeetleOtaStage::Failed;
  }
  bool retryCurrent() {
    if (stage_ == BeetleOtaStage::UpdateMain)
      stage_ = BeetleOtaStage::DiscoverMain;
    else if (stage_ == BeetleOtaStage::UpdateSub)
      stage_ = BeetleOtaStage::DiscoverSub;
    else if (stage_ != BeetleOtaStage::DiscoverMain &&
             stage_ != BeetleOtaStage::DiscoverSub)
      return false;
    return true;
  }
  void fail() { stage_ = BeetleOtaStage::Failed; }
  void reset() { stage_ = BeetleOtaStage::Idle; }
  BeetleOtaStage stage() const { return stage_; }
  bool active() const { return stage_ != BeetleOtaStage::Idle; }
 private:
  BeetleOtaStage stage_ = BeetleOtaStage::Idle;
};

}  // namespace tagmachine
