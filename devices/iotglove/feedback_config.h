#pragma once

#include "game_model.h"

namespace iotglove {
namespace feedback_config {

// Firmware-only tuning. Choose Off, Short1, Long1 or Short2 for each transition.
// A transition compares role/deviceState/phase, never LED count or chip input.
enum class Pattern : uint8_t { Off, Short1, Long1, Short2 };

struct Settings {
  uint32_t shortMs = 150;
  uint32_t longMs = 300;
  uint32_t doubleGapMs = 100;

  Pattern onSetting = Pattern::Short1;
  Pattern onReady = Pattern::Short1;
  Pattern onExploration = Pattern::Short1;
  Pattern onPlayer = Pattern::Short1;
  Pattern onGhost = Pattern::Long1;
  Pattern onTaggerBlink = Pattern::Short2;
  Pattern onTaggerActive = Pattern::Long1;
  Pattern onEnded = Pattern::Long1;

  // Preserve the existing explicit-event timings: 300 and 150/100/150 ms.
  Pattern onRemoved = Pattern::Long1;
  Pattern onFound = Pattern::Short2;
};

struct Schedule {
  uint32_t firstOn = 0;
  uint32_t gap = 0;
  uint32_t secondOn = 0;
  uint32_t total = 0;
};

inline Schedule schedule(Pattern pattern, const Settings& settings) {
  Schedule out;
  switch (pattern) {
    case Pattern::Short1: out.firstOn = settings.shortMs; break;
    case Pattern::Long1: out.firstOn = settings.longMs; break;
    case Pattern::Short2:
      out.firstOn = out.secondOn = settings.shortMs;
      out.gap = settings.doubleGapMs;
      break;
    case Pattern::Off: return out;
  }
  const uint64_t total = uint64_t(out.firstOn) + out.gap + out.secondOn;
  // Zero disables the pulse; reject overflowing/ambiguous millis durations.
  if (!out.firstOn || total > 0x7fffffffULL) return Schedule{};
  out.total = uint32_t(total);
  return out;
}

inline bool motorOn(const Schedule& pattern, uint32_t elapsed) {
  return elapsed < pattern.firstOn ||
      (elapsed >= pattern.firstOn + pattern.gap && elapsed < pattern.total);
}

inline Pattern forState(const Feedback& state, const Settings& settings) {
  if (state.phase == Phase::Ended || state.deviceState == DeviceState::Ended)
    return settings.onEnded;
  if (state.phase == Phase::Exploration || state.deviceState == DeviceState::Exploration)
    return settings.onExploration;
  if (state.phase == Phase::Unknown) return Pattern::Off;
  if (state.deviceState == DeviceState::Setting) return settings.onSetting;
  if (state.deviceState == DeviceState::Ready) return settings.onReady;
  if (state.role == Role::Tagger && state.deviceState == DeviceState::Blink)
    return settings.onTaggerBlink;
  if (state.role == Role::Tagger && state.deviceState == DeviceState::Activate)
    return settings.onTaggerActive;
  if (state.phase == Phase::Setting) return settings.onSetting;
  if (state.phase == Phase::Ready) return settings.onReady;
  switch (state.role) {
    case Role::Player: return settings.onPlayer;
    case Role::Ghost: return settings.onGhost;
    case Role::Tagger: return settings.onTaggerActive;
    case Role::Neutral: return Pattern::Off;
  }
  return Pattern::Off;
}

}  // namespace feedback_config
}  // namespace iotglove
