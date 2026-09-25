#include "card_ndef.h"
#include <stddef.h>
#include <string.h>

namespace CardNdef {
namespace {

bool fail(Image &image, const char *&error, const char *message)
{
  memset(&image, 0, sizeof(image));
  error = message;
  return false;
}

bool boundedLength(const char *text, size_t maximum, size_t &length)
{
  if (!text) return false;
  for (length = 0; length <= maximum; ++length)
    if (!text[length]) return true;
  return false;
}

bool alpha(unsigned char c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool digit(unsigned char c) { return c >= '0' && c <= '9'; }

bool wordByte(unsigned char c)
{
  // Conservatively treat a neighboring UTF-8 character as a word character.
  return alpha(c) || digit(c) || c >= 0x80;
}

bool gameCode(const char *text, size_t length)
{
  return length == 4 && text[0] == 'G' && digit(text[1]) &&
         text[2] == 'P' && digit(text[3]);
}

// Reject overlong encodings, surrogates, truncated sequences, out-of-range
// scalars and controls. UTF-8 is copied intact; no normalization is attempted.
bool cleanUtf8(const char *text, size_t length)
{
  for (size_t i = 0; i < length;) {
    unsigned char first = static_cast<unsigned char>(text[i++]);
    uint32_t cp = first;
    size_t extra = 0;
    uint32_t minimum = 0;
    if (first < 0x80) {
      if (first < 0x20 || first == 0x7F) return false;
      continue;
    }
    if (first >= 0xC2 && first <= 0xDF) { extra = 1; cp &= 0x1F; minimum = 0x80; }
    else if (first >= 0xE0 && first <= 0xEF) { extra = 2; cp &= 0x0F; minimum = 0x800; }
    else if (first >= 0xF0 && first <= 0xF4) { extra = 3; cp &= 7; minimum = 0x10000; }
    else return false;
    if (length - i < extra) return false;
    for (size_t n = 0; n < extra; ++n) {
      unsigned char next = static_cast<unsigned char>(text[i++]);
      if ((next & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (next & 0x3F);
    }
    if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) ||
        (cp >= 0x80 && cp <= 0x9F) || (cp >= 0xFDD0 && cp <= 0xFDEF) ||
        (cp & 0xFFFF) >= 0xFFFE || cp == 0x2028 || cp == 0x2029)
      return false;
  }
  return true;
}

bool templatePosition(const char *pattern, size_t length, size_t &position)
{
  bool found = false;
  for (size_t i = 0; i < length; ++i) {
    if (pattern[i] == '{') {
      if (found || length - i < 6 || memcmp(pattern + i, "{code}", 6)) return false;
      position = i;
      found = true;
      i += 5;
    } else if (pattern[i] == '}') return false;
  }
  return found;
}

bool starts(const char *text, const char *prefix)
{
  return strncmp(text, prefix, strlen(prefix)) == 0;
}

bool hasScheme(const char *text, size_t length)
{
  if (!length || !alpha(text[0])) return false;
  for (size_t i = 1; i < length; ++i) {
    unsigned char c = text[i];
    if (c == ':') return true;
    if (!alpha(c) && !digit(c) && c != '+' && c != '-' && c != '.') return false;
  }
  return false;
}

// These four NFC Forum URI identifiers are the offered prefix choices.
const char *prefixText(Prefix prefix)
{
  switch (prefix) {
    case Prefix::HttpWww: return "http://www.";
    case Prefix::HttpsWww: return "https://www.";
    case Prefix::Http: return "http://";
    case Prefix::Https: return "https://";
    default: return "";
  }
}

uint8_t prefixIdentifier(Prefix prefix)
{
  switch (prefix) {
    case Prefix::HttpWww: return 1;
    case Prefix::HttpsWww: return 2;
    case Prefix::Http: return 3;
    case Prefix::Https: return 4;
    default: return 0;
  }
}

bool cleanUri(const char *text, size_t length)
{
  for (size_t i = 0; i < length; ++i) {
    unsigned char c = text[i];
    if (c == ' ' || c == '"' || c == '<' || c == '>' || c == '\\' ||
        c == '^' || c == '`' || c == '{' || c == '}' || c == '|') return false;
    if (c == '%') {
      if (length - i < 3) return false;
      for (size_t n = 1; n <= 2; ++n) {
        char h = text[i + n];
        if (!digit(h) && !(h >= 'a' && h <= 'f') && !(h >= 'A' && h <= 'F')) return false;
      }
      i += 2;
    }
  }
  // Full URLs use the explicitly supported HTTP(S) schemes. Incomplete
  // schemes, protocol-relative URLs and empty authorities are rejected.
  if (starts(text, "//")) return false;
  const char *authority = nullptr;
  if (starts(text, "https://")) authority = text + 8;
  else if (starts(text, "http://")) authority = text + 7;
  else if (hasScheme(text, length) || strstr(text, "://")) return false;
  if (authority) {
    size_t hostLength = strcspn(authority, "/?#");
    if (!hostLength || memchr(authority, '@', hostLength)) return false;
  }
  return true;
}

size_t findCode(const char *content, size_t length, size_t &position)
{
  size_t count = 0;
  for (size_t i = 0; i + 4 <= length; ++i) {
    if (!gameCode(content + i, 4)) continue;
    if (i && wordByte(content[i - 1])) continue;
    if (i + 4 < length && wordByte(content[i + 4])) continue;
    position = i;
    ++count;
  }
  return count;
}

} // namespace

bool validateSettings(const Settings &settings, const char *&error)
{
  error = nullptr;
  if ((settings.format != Format::Uri && settings.format != Format::Text) ||
      (settings.prefix != Prefix::Auto && settings.prefix != Prefix::None &&
       settings.prefix != Prefix::HttpWww && settings.prefix != Prefix::HttpsWww &&
       settings.prefix != Prefix::Http && settings.prefix != Prefix::Https) ||
      (settings.layout != Layout::Game && settings.layout != Layout::Standard))
    error = "invalid_settings";
  if (error) return false;

  size_t patternLength = 0, placeholder = 0;
  if (!boundedLength(settings.pattern, 96, patternLength)) error = "pattern_too_long";
  else if (!cleanUtf8(settings.pattern, patternLength)) error = "invalid_pattern_utf8";
  else if (!templatePosition(settings.pattern, patternLength, placeholder)) error = "expected_one_code_placeholder";
  return error == nullptr;
}

bool encode(const Settings &settings, const char *input, Image &image,
            const char *&error)
{
  memset(&image, 0, sizeof(image));
  if (!validateSettings(settings, error)) return false;
  const size_t patternLength = strlen(settings.pattern);
  const size_t placeholder = static_cast<size_t>(strstr(settings.pattern, "{code}") - settings.pattern);
  size_t inputLength = 0;
  if (!input) return fail(image, error, "missing_input");
  if (!boundedLength(input, 128, inputLength)) return fail(image, error, "input_too_long");
  if (!inputLength) return fail(image, error, "empty_input");
  if (!cleanUtf8(input, inputLength)) return fail(image, error, "invalid_input_utf8");

  char content[129] = {};
  size_t contentLength = inputLength;
  if (settings.format == Format::Text || gameCode(input, inputLength)) {
    contentLength += patternLength - 6;
    if (contentLength > 128) return fail(image, error, "content_too_long");
    memcpy(content, settings.pattern, placeholder);
    memcpy(content + placeholder, input, inputLength);
    memcpy(content + placeholder + inputLength, settings.pattern + placeholder + 6,
           patternLength - placeholder - 6);
  } else memcpy(content, input, inputLength);

  Prefix selected = Prefix::None;
  if (settings.format == Format::Uri) {
    if (!cleanUri(content, contentLength)) return fail(image, error, "invalid_uri");
    if (settings.prefix != Prefix::Auto && settings.prefix != Prefix::None) {
      selected = settings.prefix;
      const char *prefix = prefixText(selected);
      if (hasScheme(content, contentLength)) {
        if (!starts(content, prefix)) return fail(image, error, "prefix_mismatch");
      } else {
        size_t prefixLength = strlen(prefix);
        // A bare www. hostname already carries the host part of a www prefix.
        if ((selected == Prefix::HttpWww || selected == Prefix::HttpsWww) && starts(content, "www."))
          prefixLength -= 4;
        if (contentLength + prefixLength > 128) return fail(image, error, "content_too_long");
        memmove(content + prefixLength, content, contentLength + 1);
        memcpy(content, prefix, prefixLength);
        contentLength += prefixLength;
      }
    } else if (settings.prefix == Prefix::Auto) {
      if (starts(content, "https://www.")) selected = Prefix::HttpsWww;
      else if (starts(content, "http://www.")) selected = Prefix::HttpWww;
      else if (starts(content, "https://")) selected = Prefix::Https;
      else if (starts(content, "http://")) selected = Prefix::Http;
    }
    if (!cleanUri(content, contentLength)) return fail(image, error, "invalid_uri");
  }

  const size_t omitted = strlen(prefixText(selected));
  const size_t bodyLength = contentLength - omitted;
  const size_t metadataLength = settings.format == Format::Uri ? 1 : 3;
  const size_t payloadLength = metadataLength + bodyLength;
  const size_t recordLength = 4 + payloadLength;
  size_t codePosition = 0;
  const size_t codes = findCode(content, contentLength, codePosition);
  size_t padding = 5; // Reserved for model-specific NTAG213 lock metadata.
  if (settings.layout == Layout::Game) {
    if (settings.format == Format::Text) return fail(image, error, "game_layout_requires_uri");
    if (codes != 1) return fail(image, error, codes ? "ambiguous_game_code" : "missing_game_code");
    if (codePosition < omitted) return fail(image, error, "prefix_consumes_game_code");
    const size_t storedCodeOffset = 2 + 4 + metadataLength + codePosition - omitted;
    if (storedCodeOffset > 12) return fail(image, error, "game_code_too_late");
    padding = 12 - storedCodeOffset;
    if (padding < 5) return fail(image, error, "game_code_not_at_uri_start");
  }
  const size_t unpaddedSize = padding + 2 + recordLength + 1;
  const size_t size = (unpaddedSize + 3) & ~size_t(3);
  if (size > sizeof(image.bytes)) return fail(image, error, "image_too_large");

  size_t at = padding; // Zero bytes before 0x03 are Type 2 NULL TLVs.
  image.bytes[at++] = 0x03;
  image.bytes[at++] = static_cast<uint8_t>(recordLength);
  image.bytes[at++] = 0xD1; // MB | ME | SR | well-known TNF.
  image.bytes[at++] = 1;
  image.bytes[at++] = static_cast<uint8_t>(payloadLength);
  image.bytes[at++] = settings.format == Format::Uri ? 'U' : 'T';
  if (settings.format == Format::Uri) image.bytes[at++] = prefixIdentifier(selected);
  else {
    image.bytes[at++] = 2; // UTF-8 status; two-byte language code "en".
    image.bytes[at++] = 'e';
    image.bytes[at++] = 'n';
  }
  memcpy(image.bytes + at, content + omitted, bodyLength);
  image.bytes[at + bodyLength] = 0xFE;
  image.size = static_cast<uint16_t>(size);
  memcpy(image.content, content, contentLength + 1);
  if (codes == 1) memcpy(image.code, content + codePosition, 4);
  image.prefixCode = prefixIdentifier(selected);
  return true;
}

} // namespace CardNdef
