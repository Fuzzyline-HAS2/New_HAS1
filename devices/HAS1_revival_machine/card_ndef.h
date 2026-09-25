#ifndef REVIVAL_CARD_NDEF_H
#define REVIVAL_CARD_NDEF_H

#include <stdint.h>

namespace CardNdef {

enum class Format { Uri, Text };
enum class Prefix { Auto, None, HttpWww, HttpsWww, Http, Https };
enum class Layout { Game, Standard };

struct Settings {
  Format format = Format::Uri;
  Prefix prefix = Prefix::Auto;
  Layout layout = Layout::Game;
  char pattern[97] = "https://{code}.p.fuzzyline.io";
};

struct Image {
  // Type 2 user data beginning at page 4, padded to a four-byte page boundary.
  // At least five leading NULL TLVs reserve space for the NTAG213 lock TLV.
  // The device writer replaces those five bytes after identifying that model.
  uint8_t bytes[144];
  uint16_t size;
  char content[129];
  char code[5];
  uint8_t prefixCode;
};

// Syntax only: enum values, bounded UTF-8 pattern and exactly one placeholder.
// Layout/prefix compatibility depends on input and is checked by encode().
bool validateSettings(const Settings &settings, const char *&error);

// No allocation or I/O. On failure, image is entirely zero and error is a
// static string. Inputs are bounded, single-line UTF-8; controls are rejected.
// URI: only an exact GdigitPdigit input expands the template; other input is
// literal. Text: every input expands the single {code} placeholder.
// Game succeeds only with one standalone GdigitPdigit in the actual payload,
// aligned at bytes[12..15] (page 7). The five-byte metadata reservation means
// Game requires URI content beginning with the code after prefix compression;
// Text requires Standard layout. Standard does not promise game compatibility.
bool encode(const Settings &settings, const char *input, Image &image,
            const char *&error);

} // namespace CardNdef
#endif
