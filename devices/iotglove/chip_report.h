#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace iotglove {
// Absolute physical state only: no game role, phase or device_state is involved.
class ChipReportPolicy {
 public:
  static constexpr uint32_t kRetryMs = 5000;
  struct Request {
    char device[24] = {};
    bool present = false;
    uint32_t sequence = 0;
    uint32_t identity = 0;
  };

  static bool liveDevice(const char* name) {
    if (!name) return false;
    const size_t length = strlen(name);
    if (length < 4 || length > 6 || name[0] != 'G' ||
        (name[1] != '1' && name[1] != '2') || name[2] != 'P') return false;
    for (size_t i = 3; i < length; ++i)
      if (name[i] < '0' || name[i] > '9') return false;
    return true;
  }

  void observe(bool present) { observed_ = true; desired_ = present; }
  void bind(const char* device, bool serverReady, int serverValue) {
    if (!liveDevice(device)) { disconnect(); return; }
    if (identityKnown_ && !strcmp(device_, device)) {
      // A server-only restart or external life_chip change invalidates the
      // previous confirmation, even when the physical input did not change.
      if (!serverReady || serverValue < 0 || serverValue > 1 ||
          (acknowledged_ && acknowledgedValue_ != (serverValue != 0)))
        acknowledged_ = false;
      return;
    }
    strcpy(device_, device);
    identityKnown_ = true;
    acknowledged_ = false;
    ++identity_;
  }
  void disconnect() {
    if (identityKnown_) ++identity_;
    identityKnown_ = false;
    acknowledged_ = false;
    device_[0] = 0;
  }
  bool due(uint32_t now) const {
    return observed_ && identityKnown_ && !inFlight_ &&
        (!acknowledged_ || acknowledgedValue_ != desired_) &&
        (!retry_ || uint32_t(now - failedAt_) >= kRetryMs);
  }
  bool begin(uint32_t now, Request& request) {
    if (!due(now)) return false;
    request = Request{};
    strcpy(request.device, device_);
    request.present = desired_;
    request.identity = identity_;
    request.sequence = ++sequence_;
    inFlight_ = true;
    // Once a write starts, even a lost response can mean the server changed.
    acknowledged_ = false;
    return true;
  }
  void finish(const Request& request, bool readBackVerified, uint32_t now) {
    if (!inFlight_ || request.sequence != sequence_) return;
    inFlight_ = false;
    if (readBackVerified && identityKnown_ && request.identity == identity_ &&
        !strcmp(request.device, device_)) {
      acknowledged_ = true;
      acknowledgedValue_ = request.present;
      retry_ = false;
    } else {
      acknowledged_ = false;
      failedAt_ = now;
      retry_ = true;
    }
  }

 private:
  char device_[24] = {};
  bool observed_ = false, desired_ = false, identityKnown_ = false;
  bool acknowledged_ = false, acknowledgedValue_ = false, inFlight_ = false, retry_ = false;
  uint32_t identity_ = 0, sequence_ = 0, failedAt_ = 0;
};
}  // namespace iotglove
