#pragma once

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "IoTGloveProtocol.h"

namespace iotglove {
namespace ota {

struct Target { uint32_t version = 0; };  // Zero selects the existing fixed release.
struct Command { Target ttgo, beetle; };
enum class ParseResult : uint8_t { NotCommand, Valid, Malformed };

inline bool parseVersion(const char* text, uint32_t& version) {
  uint32_t parsed = 0;
  if (!text || text[0] < '1' || text[0] > '9' ||
      !wire::uint32(text, parsed) || parsed > INT32_MAX) return false;
  version = parsed;
  return true;
}

inline ParseResult parseCommand(const char* text, Command& result) {
  result = {};
  if (!text || strncmp(text, "github", 6) != 0) return ParseResult::NotCommand;
  if (strcmp(text, "github") == 0) return ParseResult::Valid;
  if (text[6] != '@') return ParseResult::Malformed;
  // github@2147483647:2147483647 is the longest valid command.
  const size_t length = strlen(text + 7);
  if (!length || length > 21) return ParseResult::Malformed;
  char versions[22];
  memcpy(versions, text + 7, length + 1);
  char* separator = strchr(versions, ':');
  if (separator) *separator++ = '\0';
  Command next;
  if (!parseVersion(versions, next.ttgo.version) ||
      (separator && !parseVersion(separator, next.beetle.version))) return ParseResult::Malformed;
  if (!separator) next.beetle = next.ttgo;
  result = next;
  return ParseResult::Valid;
}

inline bool boardName(const char* board) {
  return board && (!strcmp(board, "iotglove") || !strcmp(board, "iotglove_beetle"));
}

inline bool archiveBaseUrl(const char* board, uint32_t version, char* out, size_t size) {
  if (!out || !size) return false;
  out[0] = '\0';
  if (!boardName(board) || !version || version > INT32_MAX) return false;
  const int count = snprintf(out, size,
      "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/%s-v%lu/",
      board, static_cast<unsigned long>(version));
  if (count < 0 || static_cast<size_t>(count) >= size) { out[0] = '\0'; return false; }
  return true;
}

// HMAC(ota.txt) authenticates board, version, partition and the image HMAC.
// Never trust version.txt to select or acknowledge a pinned image.
struct Metadata {
  char board[16] = {};
  uint32_t version = 0, partition = 0;
  uint8_t imageHmac[32] = {};
};
constexpr size_t kMetadataCapacity = 160;

inline int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

inline bool parseMetadata(const char* bytes, size_t length, Metadata& result) {
  if (!bytes || !length || length >= kMetadataCapacity || bytes[length - 1] != '\n' ||
      memchr(bytes, '\0', length)) return false;
  char buffer[kMetadataCapacity];
  memcpy(buffer, bytes, length);
  buffer[length - 1] = '\0';
  char* fields[6] = {buffer};
  size_t count = 1;
  for (char* p = buffer; *p; ++p) {
    if (*p != '|') continue;
    if (count >= 6) return false;
    *p = '\0'; fields[count++] = p + 1;
  }
  Metadata next;
  if (count != 6 || strcmp(fields[0], "IGOTA1") || !boardName(fields[1]) ||
      !parseVersion(fields[2], next.version) || !parseVersion(fields[3], next.partition) ||
      strcmp(fields[4], "min_spiffs") || strlen(fields[5]) != 64) return false;
  strcpy(next.board, fields[1]);
  for (size_t i = 0; i < 32; ++i) {
    const int high = hexDigit(fields[5][2 * i]), low = hexDigit(fields[5][2 * i + 1]);
    if (high < 0 || low < 0) return false;
    next.imageHmac[i] = static_cast<uint8_t>((high << 4) | low);
  }
  result = next;
  return true;
}

inline bool matchesMetadata(const Metadata& metadata, const char* board,
                            uint32_t version, uint32_t partition) {
  return boardName(board) && !strcmp(metadata.board, board) &&
      metadata.version == version && metadata.partition == partition;
}

inline bool equalHmac(const uint8_t* a, const uint8_t* b) {
  uint8_t difference = 0;
  for (size_t i = 0; i < 32; ++i) difference |= a[i] ^ b[i];
  return difference == 0;
}

}  // namespace ota
}  // namespace iotglove
