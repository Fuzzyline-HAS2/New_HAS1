#ifndef HAS2_BOUNDED_TRANSPORT_H
#define HAS2_BOUNDED_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#ifdef HAS2_BT_HOST
#include <string>
#else
#include <Arduino.h>
#endif

namespace Has2BoundedTransport {
#ifdef HAS2_BT_HOST
using Payload = std::string;
#else
using Payload = String;
#endif

enum class Error : uint8_t {
  None, InvalidRequest, Connect, Write, Read, Deadline, HeaderTooLarge,
  InvalidHeader, UnsupportedEncoding, BodyTooLarge, Truncated, InvalidBody, NoMemory
};
enum class Phase : uint8_t { Connect, Write, Header, Body, Close, Done };
struct Timings {
  uint32_t connectMs = 0, writeMs = 0, firstByteMs = 0;
  uint32_t headerMs = 0, bodyMs = 0, closeMs = 0, totalMs = 0;
};
struct Options {
  uint32_t deadlineMs = 2000;
  uint32_t bodyDeadlineMs = 1500;
  size_t maxBodyBytes = 16 * 1024;
  bool requireContentLength = false; // Wire contract: present and strictly positive.
  void (*progressHook)() = nullptr;
  // Owner-worker only, nonblocking POD publication; no JSON/game/RTC work.
  // Done marks a finished exchange (including failure); Result.phase preserves
  // the failure stage. Invoked once per actual phase entry, not per byte.
  void (*phaseHook)(Phase) = nullptr;
};
struct Result {
  int status = 0;
  Payload payload;
  Error error = Error::None;
  Phase phase = Phase::Connect;
  int socketError = 0;
  Timings timings;
  // HTTP error statuses are completed responses; inspect status separately.
  bool ok() const { return error == Error::None; }
};

// Plain HTTP GET only. Host must be a numeric IPv4 address; path must already
// be URL-encoded. No DNS, TLS, redirects, connection reuse, or hidden retries.
Result get(const char *host, uint16_t port, const char *path,
           const Options &options = Options{});
const char *errorName(Error error);
const char *phaseName(Phase phase);

namespace detail {
// A single owner uses this interface. No socket access from another task.
// read: >0 bytes, 0 EOF, -1 would-block, -2 fatal. write: >0, -1, -2.
class Io {
 public:
  enum class Connected { Ready, Pending, Failed };
  virtual ~Io() = default;
  virtual uint32_t now() = 0;
  virtual Connected beginConnect(const char *host, uint16_t port) = 0;
  virtual Connected pollConnect() = 0;
  virtual int write(const uint8_t *data, size_t length) = 0;
  virtual int read(uint8_t *data, size_t length) = 0;
  virtual int lastError() const = 0;
  virtual void idle() = 0;
  virtual void close() = 0;
};
// Portable protocol engine, exposed for deterministic fault tests.
Result run(Io &io, const char *host, uint16_t port, const char *path,
           const Options &options);
} // namespace detail
} // namespace Has2BoundedTransport
#endif
