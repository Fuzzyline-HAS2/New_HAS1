#pragma once

#include "game_state.h"

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
  Pattern onPhoto = Pattern::Short1;
  Pattern onPlayer = Pattern::Short1;
  Pattern onGhost = Pattern::Long1;
  Pattern onTaggerBlink = Pattern::Short2;
  Pattern onTaggerActive = Pattern::Long1;
  Pattern onEnded = Pattern::Long1;

  // Preserve the existing explicit-event timings: 300 and 150/100/150 ms.
  Pattern onRemoved = Pattern::Long1;
  Pattern onFound = Pattern::Short2;

  // Operator vibe commands (server vibe 12..17) are tuned apart from state patterns.
  uint32_t commandShortMs = 200;
  uint32_t commandLongMs = 600;
  uint32_t commandGapMs = 200;
};

// Server vibe values that are operator commands rather than proximity levels (0/1/3).
constexpr uint8_t kVibeMute = 10;          // Hold: motor never runs.
constexpr uint8_t kVibeOn = 11;            // Hold: motor runs continuously.
constexpr uint8_t kVibeCommandFirst = 12;  // 12..14 short x1..3, 15..17 long x1..3, once per edge.
constexpr uint8_t kVibeCommandLast = 17;

inline bool isCommand(uint8_t vibe) { return vibe >= kVibeCommandFirst && vibe <= kVibeCommandLast; }

// A pulse train: `count` pulses of `onMs`, separated by `gapMs`. `total` ends with the last ON.
struct Schedule {
  uint32_t onMs = 0;
  uint32_t gapMs = 0;
  uint32_t count = 0;
  uint32_t total = 0;
};

inline Schedule pulses(uint32_t onMs, uint32_t gapMs, uint32_t count) {
  Schedule out;
  // Zero disables the pulse; reject overflowing/ambiguous millis durations.
  if (!onMs || !count) return out;
  const uint64_t total = uint64_t(onMs) * count + uint64_t(gapMs) * (count - 1);
  if (total > 0x7fffffffULL) return out;
  out.onMs = onMs;
  out.gapMs = gapMs;
  out.count = count;
  out.total = uint32_t(total);
  return out;
}

inline Schedule schedule(Pattern pattern, const Settings& settings) {
  switch (pattern) {
    case Pattern::Short1: return pulses(settings.shortMs, 0, 1);
    case Pattern::Long1: return pulses(settings.longMs, 0, 1);
    case Pattern::Short2: return pulses(settings.shortMs, settings.doubleGapMs, 2);
    case Pattern::Off: break;
  }
  return Schedule{};
}

inline Schedule commandSchedule(uint8_t vibe, const Settings& settings) {
  if (vibe >= 12 && vibe <= 14) return pulses(settings.commandShortMs, settings.commandGapMs, uint32_t(vibe - 11));
  if (vibe >= 15 && vibe <= 17) return pulses(settings.commandLongMs, settings.commandGapMs, uint32_t(vibe - 14));
  return Schedule{};
}

inline bool motorOn(const Schedule& pattern, uint32_t elapsed) {
  // An empty schedule has total 0, so the division below never sees a zero period.
  return elapsed < pattern.total && elapsed % (pattern.onMs + pattern.gapMs) < pattern.onMs;
}

inline Pattern forState(const Feedback& state, const Settings& settings) {
  if (state.phase == Phase::Ended || state.deviceState == DeviceState::Ended)
    return settings.onEnded;
  if (state.phase == Phase::Photo || state.deviceState == DeviceState::Photo)
    return settings.onPhoto;
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
