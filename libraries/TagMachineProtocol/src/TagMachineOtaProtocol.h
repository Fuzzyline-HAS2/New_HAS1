#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace tagmachine {
namespace ota_wire {

constexpr uint32_t kProtocolVersion = 2;
constexpr size_t kMaxLine = 80;

enum class CommandType : uint8_t { None, Query, Update };
enum class ResponseType : uint8_t { None, Version, Outcome };

// Single-character outcomes keep parsing small while retaining an unambiguous
// request identity. Only Updated and Skipped are successful terminal results.
enum class Outcome : char {
  Accepted = 'A',
  Flashing = 'P',
  Updated = 'U',
  Skipped = 'S',
  Failed = 'F',
  PeerBusy = 'B',
  Disabled = 'D',
};

struct Command {
  CommandType type = CommandType::None;
  uint32_t request = 0;
  // Zero selects the signed latest-channel manifest. A nonzero update target
  // binds the request to an immutable version archive.
  uint32_t targetVersion = 0;
};

struct Response {
  ResponseType type = ResponseType::None;
  uint32_t request = 0;
  uint32_t protocol = 0;
  uint32_t firmware = 0;
  uint32_t partition = 0;
  uint32_t boot = 0;
  Outcome outcome = Outcome::Failed;
};

inline bool parseUint32(const char* first, const char* last, uint32_t& result) {
  if (!first || !last || first == last) return false;
  uint32_t value = 0;
  for (const char* p = first; p != last; ++p) {
    if (*p < '0' || *p > '9') return false;
    const uint32_t digit = static_cast<uint32_t>(*p - '0');
    if (value > (UINT32_MAX - digit) / 10U) return false;
    value = value * 10U + digit;
  }
  result = value;
  return true;
}

inline bool nextField(const char*& cursor, const char* end,
                      const char*& first, const char*& last) {
  if (!cursor || cursor > end) return false;
  first = cursor;
  while (cursor != end && *cursor != ':') ++cursor;
  last = cursor;
  if (cursor != end) ++cursor;
  return first != last;
}

inline bool parseCommand(const char* line, size_t length, Command& result) {
  result = {};
  if (!line || length < 3 || length > kMaxLine || line[1] != ':') return false;
  CommandType type = CommandType::None;
  if (line[0] == 'Q') type = CommandType::Query;
  else if (line[0] == 'U') type = CommandType::Update;
  else return false;
  const char* separator = static_cast<const char*>(
      memchr(line + 2, ':', length - 2));
  const char* requestEnd = separator ? separator : line + length;
  uint32_t request = 0, targetVersion = 0;
  if (!parseUint32(line + 2, requestEnd, request) || request == 0) return false;
  if (separator) {
    if (type != CommandType::Update ||
        !parseUint32(separator + 1, line + length, targetVersion) ||
        targetVersion == 0 || targetVersion > INT32_MAX) return false;
  }
  result.type = type;
  result.request = request;
  result.targetVersion = targetVersion;
  return true;
}

inline bool parseResponse(const char* line, size_t length, Response& result) {
  result = {};
  if (!line || length < 3 || length > kMaxLine || line[0] != 'R' || line[2] != ':')
    return false;

  const char* cursor = line + 3;
  const char* end = line + length;
  const char* first = nullptr;
  const char* last = nullptr;
  Response next;
  next.type = line[1] == 'V' ? ResponseType::Version :
              (line[1] == 'O' ? ResponseType::Outcome : ResponseType::None);
  if (next.type == ResponseType::None ||
      !nextField(cursor, end, first, last) || !parseUint32(first, last, next.request) ||
      next.request == 0) return false;

  if (next.type == ResponseType::Version) {
    if (!nextField(cursor, end, first, last) || !parseUint32(first, last, next.protocol))
      return false;
  } else {
    if (!nextField(cursor, end, first, last) || last - first != 1) return false;
    const char code = *first;
    if (code != static_cast<char>(Outcome::Accepted) &&
        code != static_cast<char>(Outcome::Flashing) &&
        code != static_cast<char>(Outcome::Updated) &&
        code != static_cast<char>(Outcome::Skipped) &&
        code != static_cast<char>(Outcome::Failed) &&
        code != static_cast<char>(Outcome::PeerBusy) &&
        code != static_cast<char>(Outcome::Disabled)) return false;
    next.outcome = static_cast<Outcome>(code);
  }

  if (!nextField(cursor, end, first, last) || !parseUint32(first, last, next.firmware) ||
      !nextField(cursor, end, first, last) || !parseUint32(first, last, next.partition) ||
      !nextField(cursor, end, first, last) || !parseUint32(first, last, next.boot) ||
      last != end || next.firmware == 0 || next.partition == 0 || next.boot == 0)
    return false;
  result = next;
  return true;
}

inline size_t formatCommand(char* out, size_t capacity, CommandType type,
                            uint32_t request, uint32_t targetVersion = 0) {
  if (!out || !capacity || !request ||
      (type != CommandType::Query && type != CommandType::Update) ||
      (targetVersion && (type != CommandType::Update || targetVersion > INT32_MAX))) return 0;
  const int count = targetVersion
      ? snprintf(out, capacity, "U:%lu:%lu\n", static_cast<unsigned long>(request),
                 static_cast<unsigned long>(targetVersion))
      : snprintf(out, capacity, "%c:%lu\n", type == CommandType::Query ? 'Q' : 'U',
                 static_cast<unsigned long>(request));
  return count > 0 && static_cast<size_t>(count) < capacity ? static_cast<size_t>(count) : 0;
}

inline size_t formatVersion(char* out, size_t capacity, uint32_t request,
                            uint32_t firmware, uint32_t partition, uint32_t boot) {
  if (!out || !capacity || !request || !firmware || !partition || !boot) return 0;
  const int count = snprintf(out, capacity, "RV:%lu:%lu:%lu:%lu:%lu\n",
      static_cast<unsigned long>(request), static_cast<unsigned long>(kProtocolVersion),
      static_cast<unsigned long>(firmware), static_cast<unsigned long>(partition),
      static_cast<unsigned long>(boot));
  return count > 0 && static_cast<size_t>(count) < capacity ? static_cast<size_t>(count) : 0;
}

inline size_t formatOutcome(char* out, size_t capacity, uint32_t request, Outcome outcome,
                            uint32_t firmware, uint32_t partition, uint32_t boot) {
  if (!out || !capacity || !request || !firmware || !partition || !boot) return 0;
  const int count = snprintf(out, capacity, "RO:%lu:%c:%lu:%lu:%lu\n",
      static_cast<unsigned long>(request), static_cast<char>(outcome),
      static_cast<unsigned long>(firmware), static_cast<unsigned long>(partition),
      static_cast<unsigned long>(boot));
  return count > 0 && static_cast<size_t>(count) < capacity ? static_cast<size_t>(count) : 0;
}

inline bool isStructuredResponsePrefix(const char* line, size_t length) {
  return line && length >= 3 && line[0] == 'R' &&
      (line[1] == 'V' || line[1] == 'O') && line[2] == ':';
}

}  // namespace ota_wire
}  // namespace tagmachine
