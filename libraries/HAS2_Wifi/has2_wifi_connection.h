#ifndef HAS2_WIFI_CONNECTION_H
#define HAS2_WIFI_CONNECTION_H

#include <stdint.h>

// Single-owner, cooperative connection controller. Backend operations initiate
// driver work; no scan/connect completion is waited for here. No HTTP or reboot.
namespace Has2Connection {
inline bool scanCancellationExpired(uint32_t now, uint32_t started) {
  return static_cast<uint32_t>(now - started) >= 4000;
}
class Controller {
 public:
  enum class State : uint8_t { Disabled, Start, Station, Settle, Scan, Connect, Online, Backoff };
  void begin(uint32_t now) { state_ = State::Start; since_ = now; failures_ = 0; }
  State state() const { return state_; }
  uint32_t retryDelay() const { return retryMs_; }

  template<class Backend> bool service(uint32_t now, Backend &io) {
    switch (state_) {
      case State::Disabled: return false;
      case State::Start:
        if (!io.startStation()) { backoff(now); return false; }
        state_ = State::Station; since_ = now; return false;
      case State::Station:
        if (io.stationStarted()) { settle(now, io, true); return false; }
        if (elapsed(now, 1000)) backoff(now);
        return false;
      case State::Settle:
        // A disconnect is asynchronous even when the old status was offline.
        if (elapsed(now, 50) && !io.connected()) {
          if (scanNext_) {
            const int result = io.startScan();
            if (result == -2) { io.cancelScan(); backoff(now); return false; }
            state_ = State::Scan; since_ = now;
          } else if (io.connectCandidate()) {
            state_ = State::Connect; since_ = now;
          } else {
            nextCandidate(now, io);
          }
        } else if (elapsed(now, 1000)) {
          backoff(now);
        }
        return false;
      case State::Scan: {
        const int count = io.scanResult();
        if (count >= 0) {
          const bool found = io.selectCandidates(count);
          io.releaseScan();
          if (found) settle(now, io, false);
          else backoff(now);
        } else if (count == -2 || elapsed(now, 6000)) {
          io.cancelScan(); backoff(now);
        }
        return false;
      }
      case State::Connect:
        if (io.connected()) {
          io.onConnected(); failures_ = 0; state_ = State::Online;
          return true;
        }
        if (elapsed(now, 3000)) nextCandidate(now, io);
        return false;
      case State::Online:
        if (io.connected()) return true;
        settle(now, io, true); return false;
      case State::Backoff:
        if (elapsed(now, retryMs_)) { state_ = State::Start; since_ = now; }
        return false;
    }
    return false;
  }

 private:
  State state_ = State::Disabled;
  uint32_t since_ = 0, retryMs_ = 2000;
  uint8_t failures_ = 0;
  bool scanNext_ = true;
  bool elapsed(uint32_t now, uint32_t interval) const {
    return static_cast<uint32_t>(now - since_) >= interval;
  }
  void backoff(uint32_t now) {
    retryMs_ = 2000u << (failures_ < 2 ? failures_ : 2);
    if (failures_ < 2) ++failures_;
    state_ = State::Backoff; since_ = now;
  }
  template<class Backend> void settle(uint32_t now, Backend &io, bool scan) {
    io.disconnect(); scanNext_ = scan; state_ = State::Settle; since_ = now;
  }
  template<class Backend> void nextCandidate(uint32_t now, Backend &io) {
    if (io.advanceCandidate()) settle(now, io, false);
    else { io.disconnect(); backoff(now); }
  }
};
} // namespace Has2Connection
#endif
