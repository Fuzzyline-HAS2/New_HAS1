#pragma once

#include <IoTGloveProtocol.h>
#include "library_and_pin.h"

namespace iotglove {

enum class PeerOta : uint8_t { Idle, Waiting, Ready, Failed };

class PeerState {
 public:
  bool receive(const wire::Frame& frame, uint32_t now) {
    uint32_t a = 0, b = 0, c = 0;
    if (strcmp(frame.type, "HELLO") == 0 && frame.count == 4 &&
        strcmp(frame.args[0], "beetle") == 0 && wire::uint32(frame.args[1], a) &&
        wire::uint32(frame.args[2], b) && wire::uint32(frame.args[3], c)) {
      const bool restarted = !havePeer_ || c != bootId_;
      if (restarted) {
        locationValid_ = false; locationSequence_ = 0; peerBusy_ = false;
        haveHeartbeat_ = false;
      }
      if (ota_ == PeerOta::Ready && a != otaConfirmedFirmware_) ota_ = PeerOta::Failed;
      havePeer_ = true;
      bootId_ = c;
      firmware_ = a;
      partition_ = b;
      lastHello_ = now;
      lastSeen_ = now;
      ++acceptedFrames_;
      return true;  // Synchronize current MODE after every hello/PING response.
    }
    if (!havePeer_) return false;
    if (strcmp(frame.type, "HEART") == 0 && frame.count == 3 &&
        wire::uint32(frame.args[0], a) && wire::uint32(frame.args[1], b) && b <= 1 &&
        wire::uint32(frame.args[2], c) && c <= 1) {
      lastSeen_ = now;
      peerBusy_ = c != 0;
      lastBusyReport_ = now;
      haveHeartbeat_ = true;
      heartbeatUptime_ = a;
      heartbeatScan_ = b != 0;
      ++acceptedFrames_;
    } else if (strcmp(frame.type, "LOC") == 0 && frame.count == 4 &&
        wire::uint32(frame.args[2], a) && wire::uint32(frame.args[3], b) && b <= 1) {
      if (!frame.id || (locationSequence_ && int32_t(frame.id - locationSequence_) <= 0)) return false;
      int32_t rssi = 0;
      if (!wire::int32(frame.args[1], rssi) || rssi < -127 || rssi > 0) return false;
      locationSequence_ = frame.id;
      lastSeen_ = now;
      ++acceptedFrames_;
      locationValid_ = b == 1 && a < kLocationFreshMs && knownRoom(frame.args[0]);
      if (locationValid_) {
        strcpy(room_, frame.args[0]);
        locationAt_ = now;
        locationAge_ = a;
      }
    } else if (strcmp(frame.type, "OTA_RESULT") == 0 && frame.count == 2 &&
               frame.id == otaId_ && ota_ == PeerOta::Waiting &&
               wire::uint32(frame.args[1], a)) {
      lastSeen_ = now;
      ++acceptedFrames_;
      const char* result = frame.args[0];
      const bool targetMatches = otaTargetVersion_ == 0 || a == otaTargetVersion_;
      if (strcmp(result, "skipped") == 0) {
        ota_ = targetMatches && a == otaFirmware_ && firmware_ == a ?
            PeerOta::Ready : PeerOta::Failed;
        if (ota_ == PeerOta::Ready) otaConfirmedFirmware_ = a;
      } else if (strcmp(result, "updated") == 0) {
        if (!targetMatches || a == otaFirmware_) ota_ = PeerOta::Failed;
        // A lower pinned version is a valid rollback. In all cases require a
        // new boot HELLO reporting the same installed version; flashing is not
        // proof. A result arriving before HELLO remains pending for replay.
        else if (bootId_ != otaBootId_ && firmware_ == a) {
          otaConfirmedFirmware_ = a;
          ota_ = PeerOta::Ready;
        }
      } else if (strcmp(result, "failed") == 0 || strcmp(result, "disabled") == 0 ||
                 strcmp(result, "busy") == 0)
        ota_ = PeerOta::Failed;
      // accepted/flashing mean progress, never successful installation.
    }
    return false;
  }

  bool beginOta(uint32_t requestId, uint32_t now, uint32_t targetVersion = 0) {
    if (!online(now) || !requestId || targetVersion > INT32_MAX ||
        ota_ == PeerOta::Waiting || busy(now)) return false;
    ota_ = PeerOta::Waiting;
    otaId_ = requestId;
    otaStart_ = now;
    otaFirmware_ = firmware_;
    otaBootId_ = bootId_;
    otaTargetVersion_ = targetVersion;
    locationValid_ = false;
    return true;
  }
  void tick(uint32_t now) {
    if (ota_ == PeerOta::Waiting && uint32_t(now - otaStart_) >= kBeetleOtaTimeoutMs)
      ota_ = PeerOta::Failed;
    if (ota_ == PeerOta::Ready && !online(now)) ota_ = PeerOta::Failed;
  }
  bool online(uint32_t now) const { return havePeer_ && uint32_t(now - lastSeen_) < kLocationFreshMs; }
  bool known() const { return havePeer_; }
  uint32_t firmwareVersion() const { return firmware_; }
  uint32_t partitionVersion() const { return partition_; }
  uint32_t bootId() const { return bootId_; }
  // Age getters are meaningful only when their corresponding known flag is true.
  uint32_t lastSeenAge(uint32_t now) const { return uint32_t(now - lastSeen_); }
  uint32_t helloAge(uint32_t now) const { return uint32_t(now - lastHello_); }
  bool helloFresh(uint32_t now) const { return havePeer_ && helloAge(now) < kLocationFreshMs; }
  bool heartbeatKnown() const { return haveHeartbeat_; }
  uint32_t heartbeatAge(uint32_t now) const { return uint32_t(now - lastBusyReport_); }
  bool heartbeatFresh(uint32_t now) const {
    return haveHeartbeat_ && heartbeatAge(now) < kLocationFreshMs;
  }
  uint32_t heartbeatUptime() const { return heartbeatUptime_; }
  bool heartbeatScanEnabled() const { return heartbeatScan_; }
  bool heartbeatOtaBusy() const { return peerBusy_; }
  uint32_t acceptedFrames() const { return acceptedFrames_; }
  bool locationFresh(uint32_t now) const {
    return online(now) && locationValid_ && uint32_t(now - locationAt_) < kLocationFreshMs &&
        locationAge_ < kLocationFreshMs - uint32_t(now - locationAt_);
  }
  const char* room(uint32_t now) const { return locationFresh(now) ? room_ : ""; }
  PeerOta ota() const { return ota_; }
  bool busy(uint32_t now) const {
    return ota_ == PeerOta::Waiting ||
        (peerBusy_ && uint32_t(now - lastBusyReport_) < kLocationFreshMs);
  }
  void clearOta() { if (ota_ != PeerOta::Waiting) ota_ = PeerOta::Idle; }
  void invalidateLocation() { locationValid_ = false; }
  static bool knownRoom(const char* room) {
    static const char* const rooms[] = {"bamboo", "living", "sleeping", "toilet", "underground", "hallway"};
    for (const char* known : rooms) if (strcmp(room, known) == 0) return true;
    return false;
  }
 private:
  bool havePeer_ = false, locationValid_ = false, peerBusy_ = false;
  bool haveHeartbeat_ = false, heartbeatScan_ = false;
  char room_[40] = {};
  uint32_t bootId_ = 0, firmware_ = 0, lastSeen_ = 0;
  uint32_t partition_ = 0, lastHello_ = 0, heartbeatUptime_ = 0, acceptedFrames_ = 0;
  uint32_t locationAt_ = 0, locationAge_ = 0, locationSequence_ = 0;
  uint32_t otaId_ = 0, otaStart_ = 0, otaFirmware_ = 0;
  uint32_t otaBootId_ = 0, otaTargetVersion_ = 0;
  uint32_t otaConfirmedFirmware_ = 0;
  uint32_t lastBusyReport_ = 0;
  PeerOta ota_ = PeerOta::Idle;
};

}  // namespace iotglove
