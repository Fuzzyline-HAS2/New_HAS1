#pragma once
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace has1blackbox {
enum class EventType : uint8_t {
  Boot = 1,
  Crash,
  WifiEvent,
  WifiTry,
  WifiSummary,
  HttpSummary,
  Recovery,
  State,
  Health,
  Progress,
  Peer,
  Logger,
  Lost,
  Dropped,
  Acked
};
constexpr int32_t Unknown = INT32_MIN;
inline uint32_t crc32(const uint8_t *p, size_t n) {
  uint32_t c = ~0u;
  while (n--) {
    c ^= *p++;
    for (int i = 0; i < 8; ++i)
      c = (c >> 1) ^ ((0u - (c & 1u)) & 0xedb88320u);
  }
  return ~c;
}
inline uint32_t get32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}
inline void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    p[i] = uint8_t(v >> (8 * i));
}
struct Record {
  uint8_t bytes[64]{};
  uint32_t seq() const { return get32(bytes + 4); }
  void sequence(uint32_t v) {
    put32(bytes + 4, v);
    seal();
  }
  void seal() { put32(bytes + 60, crc32(bytes, 60)); }
  bool valid() const {
    return bytes[0] == 0xb7 && bytes[1] >= 1 && bytes[1] <= 15 &&
           bytes[2] == 0 && bytes[3] == 0 && seq() != 0 &&
           get32(bytes + 60) == crc32(bytes, 60);
  }
  bool erased() const {
    for (auto b : bytes)
      if (b != 255)
        return false;
    return true;
  }
  static Record make(EventType t, uint32_t boot, uint32_t up, int32_t a = 0,
                     int32_t b = 0, int32_t c = 0, int32_t d = 0,
                     const char *text = "") {
    Record r;
    r.bytes[0] = 0xb7;
    r.bytes[1] = uint8_t(t);
    put32(r.bytes + 8, boot);
    put32(r.bytes + 12, up);
    const int32_t v[] = {a, b, c, d};
    for (int i = 0; i < 4; ++i)
      put32(r.bytes + 16 + i * 4, uint32_t(v[i]));
    if (text)
      for (size_t i = 0; i < 27 && text[i]; ++i)
        r.bytes[32 + i] =
            (uint8_t(text[i]) >= 32 && uint8_t(text[i]) <= 126) ? text[i] : '?';
    r.seal();
    return r;
  }
  bool hex(char *out, size_t cap) const {
    if (cap < 129)
      return false;
    constexpr char h[] = "0123456789abcdef";
    for (size_t i = 0; i < 64; ++i) {
      out[i * 2] = h[bytes[i] >> 4];
      out[i * 2 + 1] = h[bytes[i] & 15];
    }
    out[128] = 0;
    return true;
  }
};
static_assert(sizeof(Record) == 64, "wire size");
} // namespace has1blackbox
