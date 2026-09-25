#include "card_ndef.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

using namespace CardNdef;
static unsigned int checks = 0;

#define CHECK(condition) do { ++checks; if (!(condition)) { \
  fprintf(stderr, "line %d: %s\n", __LINE__, #condition); assert(condition); } } while (0)

static void pattern(Settings &settings, const char *value)
{
  CHECK(strlen(value) < sizeof(settings.pattern));
  strcpy(settings.pattern, value);
}

static bool zeroImage(const Image &image)
{
  const unsigned char *bytes = reinterpret_cast<const unsigned char *>(&image);
  for (size_t i = 0; i < sizeof(image); ++i) if (bytes[i]) return false;
  return true;
}

static void rejected(const Settings &settings, const char *input, const char *expected)
{
  Image image;
  memset(&image, 0xA5, sizeof(image));
  const char *error = nullptr;
  CHECK(!encode(settings, input, image, error));
  CHECK(error != nullptr);
  CHECK(!strcmp(error, expected));
  CHECK(zeroImage(image));
}

static Image accepted(const Settings &settings, const char *input)
{
  Image image;
  memset(&image, 0xA5, sizeof(image));
  const char *error = "old_error";
  CHECK(encode(settings, input, image, error));
  CHECK(error == nullptr);
  CHECK(image.size >= 12 && image.size <= 144 && image.size % 4 == 0);
  for (size_t i = image.size; i < sizeof(image.bytes); ++i) CHECK(image.bytes[i] == 0);
  for (size_t i = 0; i < 5; ++i) CHECK(image.bytes[i] == 0);
  if (settings.layout == Layout::Game) {
    CHECK(strlen(image.code) == 4);
    CHECK(!memcmp(image.bytes + 12, image.code, 4));
  }
  return image;
}

static void goldenUri()
{
  Settings settings;
  Image image = accepted(settings, "G1P2");
  const uint8_t expected[] = {
    0,0,0,0,0, 0x03,0x18,0xD1,0x01,0x14,0x55,0x04,
    'G','1','P','2','.','p','.','f','u','z','z','y','l','i','n','e','.','i','o',0xFE
  };
  CHECK(image.size == sizeof(expected));
  CHECK(!memcmp(image.bytes, expected, sizeof(expected)));
  CHECK(!strcmp(image.content, "https://G1P2.p.fuzzyline.io"));
  CHECK(!strcmp(image.code, "G1P2"));
  CHECK(image.prefixCode == 4);

  image = accepted(settings, "https://www.G2P3.p.fuzzyline.io");
  const uint8_t expectedWww[] = {
    0,0,0,0,0, 0x03,0x18,0xD1,0x01,0x14,0x55,0x02,
    'G','2','P','3','.','p','.','f','u','z','z','y','l','i','n','e','.','i','o',0xFE
  };
  CHECK(image.size == sizeof(expectedWww));
  CHECK(!memcmp(image.bytes, expectedWww, sizeof(expectedWww)));
  CHECK(!strcmp(image.content, "https://www.G2P3.p.fuzzyline.io"));
  CHECK(image.prefixCode == 2);

  image = accepted(settings, "http://www.G3P4.example");
  CHECK(image.prefixCode == 1);
  image = accepted(settings, "http://G3P4.example");
  CHECK(image.prefixCode == 3);
}

static void goldenTextAndNone()
{
  Settings settings;
  settings.format = Format::Text;
  settings.layout = Layout::Standard;
  pattern(settings, "{code}");
  Image image = accepted(settings, "A");
  const uint8_t expected[] = {
    0,0,0,0,0, 0x03,0x08,0xD1,0x01,0x04,0x54,0x02,'e','n','A',0xFE
  };
  CHECK(image.size == sizeof(expected));
  CHECK(!memcmp(image.bytes, expected, sizeof(expected)));
  CHECK(!strcmp(image.content, "A"));
  CHECK(image.code[0] == 0 && image.prefixCode == 0);
  pattern(settings, "tag:{code}");
  image = accepted(settings, "TEST001");
  CHECK(!strcmp(image.content, "tag:TEST001"));
  settings.layout = Layout::Game;
  rejected(settings, "G1P2", "game_layout_requires_uri");

  settings = Settings();
  settings.layout = Layout::Standard;
  settings.prefix = Prefix::None;
  pattern(settings, "{code}");
  image = accepted(settings, "https://x");
  const uint8_t expectedNone[] = {
    0,0,0,0,0, 0x03,0x0E,0xD1,0x01,0x0A,0x55,0x00,
    'h','t','t','p','s',':','/','/','x',0xFE,0,0
  };
  CHECK(image.size == sizeof(expectedNone));
  CHECK(!memcmp(image.bytes, expectedNone, sizeof(expectedNone)));
}

static void prefixesAndTemplates()
{
  Settings settings;
  settings.layout = Layout::Standard;
  settings.prefix = Prefix::HttpsWww;
  Image image = accepted(settings, "example.com");
  CHECK(!strcmp(image.content, "https://www.example.com"));
  CHECK(image.prefixCode == 2);
  image = accepted(settings, "www.example.com");
  CHECK(!strcmp(image.content, "https://www.example.com"));
  image = accepted(settings, "https://www.example.com");
  CHECK(!strcmp(image.content, "https://www.example.com"));
  rejected(settings, "https://example.com", "prefix_mismatch");
  rejected(settings, "http://www.example.com", "prefix_mismatch");
  rejected(settings, "G1P2", "prefix_mismatch");
  pattern(settings, "www.{code}.example");
  image = accepted(settings, "G1P2");
  CHECK(!strcmp(image.content, "https://www.G1P2.example"));
  settings.layout = Layout::Game;
  image = accepted(settings, "G1P2");
  CHECK(image.prefixCode == 2);

  pattern(settings, "{code}.example");
  settings.prefix = Prefix::Http;
  image = accepted(settings, "G1P2");
  CHECK(!strcmp(image.content, "http://G1P2.example"));
  settings.prefix = Prefix::Https;
  image = accepted(settings, "G1P2");
  CHECK(!strcmp(image.content, "https://G1P2.example"));
  settings.prefix = Prefix::HttpWww;
  image = accepted(settings, "G1P2");
  CHECK(!strcmp(image.content, "http://www.G1P2.example"));
  settings.prefix = Prefix::None;
  image = accepted(settings, "G1P2");
  CHECK(!strcmp(image.content, "G1P2.example"));
  CHECK(image.prefixCode == 0);

  settings = Settings();
  settings.prefix = Prefix::None;
  rejected(settings, "G1P2", "game_code_too_late");
  settings.layout = Layout::Standard;
  image = accepted(settings, "G1P2");
  CHECK(!strcmp(image.content, "https://G1P2.p.fuzzyline.io"));
  CHECK(!memcmp(image.bytes + 20, "G1P2", 4));
  settings.prefix = Prefix::Auto;
  image = accepted(settings, "https://other.example/path");
  CHECK(!strcmp(image.content, "https://other.example/path"));
}

static void gameCodeRules()
{
  Settings settings;
  rejected(settings, "https://example.com/G1P2", "game_code_too_late");
  rejected(settings, "https://a/G1P2", "game_code_not_at_uri_start");
  rejected(settings, "https://G1P2.example/G3P4", "ambiguous_game_code");
  rejected(settings, "https://G1P2.example/G1P2", "ambiguous_game_code");
  rejected(settings, "https://AG1P2.example", "missing_game_code");
  rejected(settings, "https://G1P2X.example", "missing_game_code");
  rejected(settings, "https://G11P2.example", "missing_game_code");
  rejected(settings, "https://g1p2.example", "missing_game_code");
  rejected(settings, "https://G1P2\xC3\xA9.example", "missing_game_code");
  rejected(settings, "https://\xC3\xA9G1P2.example", "missing_game_code");
  rejected(settings, "https://example.com", "missing_game_code");
  Image image = accepted(settings, "https://G0P9.example");
  CHECK(!strcmp(image.code, "G0P9"));
  settings.layout = Layout::Standard;
  image = accepted(settings, "https://G1P2.example/G3P4");
  CHECK(image.code[0] == 0);
  image = accepted(settings, "https://example.com/G1P2");
  CHECK(!strcmp(image.code, "G1P2"));
}

static void settingsAndInputErrors()
{
  Settings settings;
  const char *error = "old_error";
  CHECK(validateSettings(settings, error) && !error);
  settings.prefix = Prefix::HttpsWww;
  CHECK(validateSettings(settings, error) && !error); // URI input may supply the matching prefix.
  settings.format = Format::Text;
  CHECK(validateSettings(settings, error) && !error); // Layout may be configured next.
  settings = Settings();
  rejected(settings, nullptr, "missing_input");
  rejected(settings, "", "empty_input");
  settings.format = static_cast<Format>(99);
  CHECK(!validateSettings(settings, error) && !strcmp(error, "invalid_settings"));
  rejected(settings, "G1P2", "invalid_settings");
  settings = Settings();
  settings.prefix = static_cast<Prefix>(-1);
  rejected(settings, "G1P2", "invalid_settings");
  settings = Settings();
  settings.layout = static_cast<Layout>(99);
  rejected(settings, "G1P2", "invalid_settings");
  settings = Settings();
  const char *badPatterns[] = {"", "fixed", "{Code}", "{code}{code}", "{{code}}", "{code}{", "{code}}", "{other}{code}"};
  for (size_t i = 0; i < sizeof(badPatterns) / sizeof(badPatterns[0]); ++i) {
    pattern(settings, badPatterns[i]);
    CHECK(!validateSettings(settings, error) && !strcmp(error, "expected_one_code_placeholder"));
    rejected(settings, "https://G1P2.example", "expected_one_code_placeholder");
  }
  memset(settings.pattern, 'x', sizeof(settings.pattern));
  CHECK(!validateSettings(settings, error) && !strcmp(error, "pattern_too_long"));
  rejected(settings, "G1P2", "pattern_too_long");
  settings = Settings();
  pattern(settings, "{code}\n");
  rejected(settings, "G1P2", "invalid_pattern_utf8");
  pattern(settings, "{code}\xFF");
  rejected(settings, "G1P2", "invalid_pattern_utf8");
  settings = Settings();
  rejected(settings, "G1P2\r\nwrite G9P9", "invalid_input_utf8");
  rejected(settings, "G1P2\t", "invalid_input_utf8");
  const char *badUris[] = {
    "https://x/\"", "https://x/<x>", "https://x/{code}", "https://x/a b",
    "https://x\\evil", "https://x/^", "https://x/`", "https://x/|",
    "https://x/%", "https://x/%0", "https://x/%ZZ", "javascript:alert(1)",
    "//example.com", "https://", "http://?q=x", "https://user@example.com", "https:x"
  };
  for (size_t i = 0; i < sizeof(badUris) / sizeof(badUris[0]); ++i)
    rejected(settings, badUris[i], "invalid_uri");
  settings.layout = Layout::Standard;
  Image image = accepted(settings, "https://example.com/a%20b?q=1&next=two#end");
  CHECK(strstr(image.content, "a%20b") != nullptr);
}

static void utf8AndLimits()
{
  Settings settings;
  settings.format = Format::Text;
  settings.layout = Layout::Standard;
  pattern(settings, "{code}");
  const char valid[] = "\xED\x95\x9C\xEA\xB8\x80 \xF0\x9F\x8E\xB2";
  Image image = accepted(settings, valid);
  CHECK(!strcmp(image.content, valid));
  CHECK(image.bytes[9] == strlen(valid) + 3); // Byte length, not Unicode character count.
  const char *badUtf8[] = {
    "\x80", "\xC0\xAF", "\xC1\x80", "\xC2", "\xDF\x7F",
    "\xE0\x80\x80", "\xED\xA0\x80", "\xEF\xBF\xBE",
    "\xF0\x80\x80\x80", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80",
    "\xC2\x85", "\xE2\x80\xA8", "\xF0\x9F\x8E"
  };
  for (size_t i = 0; i < sizeof(badUtf8) / sizeof(badUtf8[0]); ++i)
    rejected(settings, badUtf8[i], "invalid_input_utf8");

  char maximum[130];
  memset(maximum, 'a', 128);
  maximum[128] = 0;
  image = accepted(settings, maximum);
  CHECK(image.size == 144 && strlen(image.content) == 128);
  CHECK(image.bytes[142] == 0xFE && image.bytes[143] == 0);
  maximum[128] = 'a'; maximum[129] = 0;
  rejected(settings, maximum, "input_too_long");
  maximum[128] = 0;
  pattern(settings, "x{code}");
  rejected(settings, maximum, "content_too_long");

  settings = Settings(); settings.layout = Layout::Standard; settings.prefix = Prefix::HttpsWww;
  rejected(settings, maximum, "content_too_long");
  settings = Settings(); pattern(settings, "{code}");
  memcpy(maximum, "G1P2.", 5);
  image = accepted(settings, maximum);
  CHECK(image.size == 144 && strlen(image.content) == 128);

  // The entire 96-byte pattern is valid and has one six-byte placeholder.
  settings.format = Format::Text; settings.layout = Layout::Standard;
  memset(settings.pattern, 'x', 90);
  memcpy(settings.pattern + 90, "{code}", 7);
  image = accepted(settings, "A");
  CHECK(strlen(image.content) == 91);
}

static void boundariesAndDeterminism()
{
  struct Guarded { uint64_t before; Image image; uint64_t after; } guarded;
  guarded.before = UINT64_C(0x1122334455667788);
  guarded.after = UINT64_C(0x8877665544332211);
  uint32_t state = 0xC0DEFACE;
  Settings settings;
  settings.layout = Layout::Standard;
  pattern(settings, "{code}");
  char input[130];
  for (size_t trial = 0; trial < 3000; ++trial) {
    state = state * 1664525U + 1013904223U;
    size_t length = state % 130;
    for (size_t i = 0; i < length; ++i) {
      state = state * 1664525U + 1013904223U;
      input[i] = static_cast<char>(1 + ((state >> 16) % 255));
    }
    input[length] = 0;
    settings.format = trial & 1 ? Format::Text : Format::Uri;
    const char *error = nullptr;
    bool ok = encode(settings, input, guarded.image, error);
    CHECK(guarded.before == UINT64_C(0x1122334455667788));
    CHECK(guarded.after == UINT64_C(0x8877665544332211));
    if (ok) {
      CHECK(error == nullptr);
      CHECK(guarded.image.size <= 144 && guarded.image.size % 4 == 0);
      CHECK(strlen(guarded.image.content) <= 128);
      Image second;
      CHECK(encode(settings, input, second, error));
      CHECK(!memcmp(&guarded.image, &second, sizeof(second)));
    } else {
      CHECK(error != nullptr);
      CHECK(zeroImage(guarded.image));
    }
  }
}

int main()
{
  goldenUri(); goldenTextAndNone(); prefixesAndTemplates(); gameCodeRules();
  settingsAndInputErrors(); utf8AndLimits(); boundariesAndDeterminism();
  printf("PASS: 7 NDEF suites, %u checks including 3000 bounded-input cases\n", checks);
}
