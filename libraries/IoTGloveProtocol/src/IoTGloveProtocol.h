#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace iotglove {
namespace wire {

constexpr size_t kMaxLine = 192;  // Includes newline; not the C terminator.
constexpr size_t kMaxArgs = 8;
constexpr size_t kArgSize = 40;
constexpr uint32_t kFrameTimeoutMs = 250;

struct Frame {
  char type[20] = {};
  uint32_t id = 0;
  uint8_t count = 0;
  char args[kMaxArgs][kArgSize] = {};
};

inline bool token(const char* value) {
  if (!value || !*value) return false;
  for (const char* p = value; *p; ++p) {
    const char c = *p;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' ||
          c == '.' || c == ':')) return false;
  }
  return true;
}

inline bool uint32(const char* text, uint32_t& result) {
  if (!text || !*text) return false;
  uint32_t value = 0;
  for (const char* p = text; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
    const uint32_t digit = static_cast<uint32_t>(*p - '0');
    if (value > (UINT32_MAX - digit) / 10U) return false;
    value = value * 10U + digit;
  }
  result = value;
  return true;
}

inline bool int32(const char* text, int32_t& result) {
  if (!text || !*text) return false;
  const bool negative = *text == '-';
  uint32_t value = 0;
  if (!uint32(text + (negative ? 1 : 0), value)) return false;
  if (value > (negative ? 2147483648U : 2147483647U)) return false;
  result = negative ? static_cast<int32_t>(-static_cast<int64_t>(value))
                    : static_cast<int32_t>(value);
  return true;
}

inline bool put(Frame& frame, const char* value) {
  if (frame.count >= kMaxArgs || !token(value) || strlen(value) >= kArgSize)
    return false;
  strcpy(frame.args[frame.count++], value);
  return true;
}

inline size_t format(char* out, size_t capacity, const Frame& frame) {
  if (!out || capacity == 0) return 0;
  out[0] = '\0';
  if (!memchr(frame.type, '\0', sizeof(frame.type)) || !token(frame.type) ||
      frame.count > kMaxArgs) return 0;
  char line[kMaxLine + 1];
  const int initial = snprintf(line, sizeof(line), "IG1|%s|%lu", frame.type,
                               static_cast<unsigned long>(frame.id));
  if (initial < 0 || static_cast<size_t>(initial) >= sizeof(line)) return 0;
  size_t length = static_cast<size_t>(initial);
  for (uint8_t i = 0; i < frame.count; ++i) {
    if (!memchr(frame.args[i], '\0', kArgSize) || !token(frame.args[i])) return 0;
    const size_t argLength = strlen(frame.args[i]);
    if (length + argLength + 2 > kMaxLine) return 0;
    line[length++] = '|';
    memcpy(line + length, frame.args[i], argLength);
    length += argLength;
  }
  if (length + 1 > kMaxLine || length + 2 > capacity) return 0;
  line[length++] = '\n';
  line[length] = '\0';
  memcpy(out, line, length + 1);
  return length;
}

class Decoder {
 public:
  // A timeout or malformed/oversize line is discarded through the next newline.
  // This prevents accepting a valid-looking suffix of a damaged frame.
  bool feed(char c, uint32_t now, Frame& result) {
    if (length_ && static_cast<uint32_t>(now - lastByte_) > kFrameTimeoutMs) {
      length_ = 0;
      discard_ = true;
    }
    lastByte_ = now;
    if (c == '\n') {
      const bool ready = !discard_ && length_ > 0;
      line_[length_] = '\0';
      length_ = 0;
      discard_ = false;
      return ready && parse(result);
    }
    if (discard_) return false;
    if (c < 32 || c > 126 || length_ >= kMaxLine - 1) {
      length_ = 0;
      discard_ = true;
      return false;
    }
    line_[length_++] = c;
    return false;
  }

  void reset() { length_ = 0; discard_ = false; }

 private:
  bool parse(Frame& result) {
    char* parts[kMaxArgs + 3];
    size_t count = 0;
    parts[count++] = line_;
    for (char* p = line_; *p; ++p) {
      if (*p != '|') continue;
      *p = '\0';
      if (count == kMaxArgs + 3) return false;
      parts[count++] = p + 1;
    }
    if (count < 3 || strcmp(parts[0], "IG1") != 0 ||
        strlen(parts[1]) >= sizeof(result.type) || !token(parts[1])) return false;
    Frame next;
    strcpy(next.type, parts[1]);
    if (!uint32(parts[2], next.id)) return false;
    for (size_t i = 3; i < count; ++i) if (!put(next, parts[i])) return false;
    result = next;
    return true;
  }

  char line_[kMaxLine + 1] = {};
  size_t length_ = 0;
  uint32_t lastByte_ = 0;
  bool discard_ = false;
};
}  // namespace wire
}  // namespace iotglove
