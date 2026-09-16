#pragma once

#include "game_model.h"

namespace iotglove {

struct Outputs {
  uint8_t red = 0, green = 0, blue = 0;
  uint8_t lit = 0;
  bool motor = false;
};

class FeedbackEngine {
 public:
  Outputs update(const Feedback& state, uint8_t vibe, bool locationFresh, uint32_t now) {
    if (state.haptic != Haptic::None) {
      pattern_ = state.haptic;
      patternStart_ = now;
    }
    if (state.display == Display::Setting || state.display == Display::Ready ||
        state.display == Display::Ended) pattern_ = Haptic::None;
    Outputs out;
    out.lit = state.lit;
    switch (state.display) {
      case Display::Setting: out.red = out.green = out.blue = 32; break;
      case Display::Ready:
      case Display::Ended: out.red = 64; break;
      case Display::Player: out.green = 64; break;
      case Display::Ghost: out.blue = 64; break;
      case Display::Tagger: out.red = 48; out.blue = 64; break;
      case Display::TaggerActive:
        if ((now / 500U) % 2U == 0) { out.red = 48; out.blue = 64; }
        break;
    }
    const uint32_t elapsed = now - patternStart_;
    if (pattern_ == Haptic::Removed && elapsed >= 300U) pattern_ = Haptic::None;
    if (pattern_ == Haptic::Found && elapsed >= 400U) pattern_ = Haptic::None;
    if (pattern_ == Haptic::Removed) out.motor = true;
    else if (pattern_ == Haptic::Found) out.motor = elapsed < 150U || elapsed >= 250U;
    else if (locationFresh && state.display == Display::Player) {
      // Initial field-tuning policy: same room double pulse; adjacent single.
      // Event haptics always override proximity, and expired location is silent.
      if (vibe == 3) { const uint32_t t = now % 1000U; out.motor = t < 100U || (t >= 200U && t < 300U); }
      else if (vibe == 1) out.motor = now % 2000U < 100U;
    }
    return out;
  }
 private:
  Haptic pattern_ = Haptic::None;
  uint32_t patternStart_ = 0;
};

}  // namespace iotglove
