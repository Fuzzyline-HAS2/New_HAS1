#pragma once
#include <stdint.h>
namespace GeneratorBlackBox {
// Caller-owned: no allocation/locks. Samples occur every loop; storage only at
// the summary interval. Subtraction intentionally supports millis rollover.
struct BlackBoxSummary {
  uint32_t previous=0, last=0, maxGap=0, loops=0;
  bool seen=false;
  bool sample(uint32_t now) {
    if (seen) { const uint32_t gap=now-previous; if(gap>maxGap) maxGap=gap; }
    else { seen=true; last=now; }
    previous=now; ++loops;
    return uint32_t(now-last)>=60000;
  }
  void emitted(uint32_t now) { last=now; maxGap=0; loops=0; }
};
struct BlackBoxRateGate {
  uint32_t last=0; bool seen=false;
  bool allow(uint32_t now,uint32_t interval=60000) {
    if(seen && uint32_t(now-last)<interval) return false;
    last=now; seen=true; return true;
  }
};
// Unknown state, active/photo game, OTA, offline or busy game traffic is not an
// upload opportunity. Logging to the bounded RAM/flash path remains enabled.
inline bool blackBoxUploadAllowed(bool known,bool active,bool photo,bool online,bool busy,bool ota) {
  return known && !active && !photo && online && !busy && !ota;
}
}  // namespace GeneratorBlackBox
