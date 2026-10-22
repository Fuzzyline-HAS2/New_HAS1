#include "has2_bounded_transport.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#ifdef HAS2_BT_HOST
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#else
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <fcntl.h>
#endif

namespace Has2BoundedTransport {
namespace {
constexpr size_t HeaderLimit = 4096;
constexpr size_t LineLimit = 1024;
constexpr size_t BodyLimit = 16 * 1024;
constexpr size_t WireLimit = 20 * 1024;
constexpr size_t PathLimit = 2048;

bool equalIgnoreCase(const char *a, const char *b) {
  while (*a && *b) {
    const char x = (*a >= 'A' && *a <= 'Z') ? char(*a + 'a' - 'A') : *a;
    const char y = (*b >= 'A' && *b <= 'Z') ? char(*b + 'a' - 'A') : *b;
    if (x != y) return false;
    ++a; ++b;
  }
  return *a == *b;
}
bool validHost(const char *host) {
  if (!host || !*host) return false;
  unsigned groups = 0;
  const char *p = host;
  while (*p) {
    unsigned n = 0, digits = 0;
    while (*p >= '0' && *p <= '9') {
      n = n * 10 + unsigned(*p++ - '0');
      if (++digits > 3 || n > 255) return false;
    }
    if (!digits || ++groups > 4) return false;
    if (!*p) break;
    if (*p++ != '.' || !*p) return false;
  }
  return groups == 4;
}
bool append(Payload &out, char c) {
#ifdef HAS2_BT_HOST
  try { out.push_back(c); return true; } catch (...) { return false; }
#else
  return out.concat(c);
#endif
}
bool reserve(Payload &out, size_t size) {
#ifdef HAS2_BT_HOST
  try { out.reserve(size); return true; } catch (...) { return false; }
#else
  return size == 0 || out.reserve(size);
#endif
}

class Engine {
 public:
  Engine(detail::Io &io, const Options &options) : io_(io), options_(options), started_(io.now()), phaseStarted_(started_) {}
  Result execute(const char *host, uint16_t port, const char *path) {
    if (!validHost(host) || !port || !path || path[0] != '/' ||
        !options_.deadlineMs || options_.deadlineMs > INT32_MAX ||
        !options_.bodyDeadlineMs || options_.bodyDeadlineMs > INT32_MAX ||
        !options_.maxBodyBytes || options_.maxBodyBytes > BodyLimit) {
      fail(Error::InvalidRequest); return finish();
    }
    size_t pathSize = 0;
    for (; path[pathSize]; ++pathSize) {
      const unsigned char c = static_cast<unsigned char>(path[pathSize]);
      if (pathSize >= PathLimit || c <= 0x20 || c >= 0x7f || c == '#') {
        fail(Error::InvalidRequest); return finish();
      }
    }
    char request[PathLimit + 256];
    const int length = snprintf(request, sizeof(request),
      "GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\nAccept-Encoding: identity\r\nUser-Agent: HAS2-bounded/1\r\n\r\n",
      path, host, unsigned(port));
    if (length <= 0 || size_t(length) >= sizeof(request)) {
      fail(Error::InvalidRequest); return finish();
    }
    enter(Phase::Connect);
    auto state = io_.beginConnect(host, port);
    while (state == detail::Io::Connected::Pending) {
      if (!wait()) return finish();
      state = io_.pollConnect();
    }
    if (!check()) return finish();
    if (state != detail::Io::Connected::Ready) { fail(Error::Connect); return finish(); }
    result_.timings.connectMs = elapsed();
    enter(Phase::Write); phaseStarted_ = io_.now();
    const uint32_t writeStarted = io_.now();
    size_t sent = 0;
    while (sent < size_t(length)) {
      if (!check()) return finish();
      const int n = io_.write(reinterpret_cast<const uint8_t *>(request) + sent, size_t(length) - sent);
      if (n > 0 && size_t(n) <= size_t(length) - sent) sent += size_t(n);
      else if (n == -1) { if (!wait()) return finish(); }
      else { fail(Error::Write); return finish(); }
    }
    result_.timings.writeMs = uint32_t(io_.now() - writeStarted);
    enter(Phase::Header); phaseStarted_ = io_.now();
    const uint32_t headerStarted = io_.now();
    if (!headers()) return finish();
    result_.timings.headerMs = uint32_t(io_.now() - headerStarted);
    enter(Phase::Body); phaseStarted_ = io_.now();
    bodyStarted_ = io_.now();
    if (result_.status != 204 && result_.status != 304) {
      if (!reserve(result_.payload, hasLength_ ? contentLength_ : options_.maxBodyBytes)) { fail(Error::NoMemory); return finish(); }
      if (chunked_) chunks(); else identity();
    }
    result_.timings.bodyMs = uint32_t(io_.now() - bodyStarted_);
    return finish();
  }
 private:
  detail::Io &io_;
  const Options &options_;
  Result result_;
  uint32_t started_, phaseStarted_, bodyStarted_ = 0;
  size_t headerBytes_ = 0, wireBytes_ = 0;
  bool firstByte_ = false, chunked_ = false, hasLength_ = false, hasEncoding_ = false;
  size_t contentLength_ = 0;
  uint8_t buffer_[512];
  size_t pos_ = 0, count_ = 0;
  bool phaseNotified_ = false;
  void enter(Phase phase) {
    const bool changed = !phaseNotified_ || result_.phase != phase;
    result_.phase = phase; phaseNotified_ = true;
    if (changed && options_.phaseHook) options_.phaseHook(phase);
  }
  uint32_t elapsed() { return uint32_t(io_.now() - started_); }
  bool fail(Error error) {
    if (result_.error == Error::None) { result_.error = error; result_.socketError = io_.lastError(); }
    return false;
  }
  bool check() {
    if (elapsed() >= options_.deadlineMs ||
        (result_.phase == Phase::Body && uint32_t(io_.now() - bodyStarted_) >= options_.bodyDeadlineMs))
      return fail(Error::Deadline);
    return true;
  }
  bool wait() {
    if (!check()) return false;
    if (options_.progressHook) options_.progressHook();
    io_.idle();
    return check();
  }
  // >0 byte, 0 EOF, -1 error. Every byte consumes the shared deadline.
  int byte(char &c) {
    for (;;) {
      if (!check()) return -1;
      if (pos_ < count_) {
        if (result_.phase == Phase::Header) {
          if (++headerBytes_ > HeaderLimit) { fail(Error::HeaderTooLarge); return -1; }
        } else if (++wireBytes_ > WireLimit) { fail(Error::BodyTooLarge); return -1; }
        c = char(buffer_[pos_++]);
        return 1;
      }
      const int n = io_.read(buffer_, sizeof(buffer_));
      if (n > 0 && size_t(n) <= sizeof(buffer_)) {
        if (!firstByte_) { firstByte_ = true; result_.timings.firstByteMs = elapsed(); }
        count_ = size_t(n); pos_ = 0;
      } else if (n == 0) return 0;
      else if (n == -1) { if (!wait()) return -1; }
      else { fail(Error::Read); return -1; }
    }
  }
  bool line(char *text, size_t capacity) {
    size_t size = 0;
    for (;;) {
      char c;
      const int n = byte(c);
      if (n != 1) { if (!n) fail(Error::Truncated); return false; }
      if (c == '\r') {
        const int next = byte(c);
        if (next != 1) { if (!next) fail(Error::Truncated); return false; }
        if (c != '\n') return fail(result_.phase == Phase::Header ? Error::InvalidHeader : Error::InvalidBody);
        text[size] = 0; return true;
      }
      if (c == '\n' || c == 0 || (static_cast<unsigned char>(c) < 0x20 && c != '\t'))
        return fail(result_.phase == Phase::Header ? Error::InvalidHeader : Error::InvalidBody);
      if (size + 1 >= capacity)
        return fail(result_.phase == Phase::Header ? Error::HeaderTooLarge : Error::InvalidBody);
      text[size++] = c;
    }
  }
  bool headers() {
    char text[LineLimit + 1];
    if (!line(text, sizeof(text))) return false;
    if ((strncmp(text, "HTTP/1.1 ", 9) && strncmp(text, "HTTP/1.0 ", 9)) || strlen(text) < 12 ||
        text[9] < '1' || text[9] > '5' || text[10] < '0' || text[10] > '9' ||
        text[11] < '0' || text[11] > '9' || (text[12] && text[12] != ' '))
      return fail(Error::InvalidHeader);
    result_.status = (text[9] - '0') * 100 + (text[10] - '0') * 10 + text[11] - '0';
    // The local PHP contract does not use interim responses or protocol upgrades.
    if (result_.status < 200) return fail(Error::InvalidHeader);
    unsigned lines = 0;
    while (true) {
      if (!line(text, sizeof(text))) return false;
      if (!*text) break;
      if (++lines > 64 || text[0] == ' ' || text[0] == '\t') return fail(Error::InvalidHeader);
      char *colon = strchr(text, ':');
      if (!colon || colon == text) return fail(Error::InvalidHeader);
      for (const char *p = text; p < colon; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        const bool token = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
          (c >= 'a' && c <= 'z') || strchr("!#$%&'*+-.^_`|~", c);
        if (!token) return fail(Error::InvalidHeader);
      }
      *colon = 0;
      char *value = colon + 1;
      while (*value == ' ' || *value == '\t') ++value;
      char *end = value + strlen(value);
      while (end > value && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
      if (equalIgnoreCase(text, "Content-Length")) {
        if (hasLength_ || !*value) return fail(Error::InvalidHeader);
        hasLength_ = true;
        for (const char *p = value; *p; ++p) {
          if (*p < '0' || *p > '9') return fail(Error::InvalidHeader);
          const size_t digit = size_t(*p - '0');
          if (digit > options_.maxBodyBytes || contentLength_ > (options_.maxBodyBytes - digit) / 10)
            return fail(Error::BodyTooLarge);
          contentLength_ = contentLength_ * 10 + digit;
        }
      } else if (equalIgnoreCase(text, "Transfer-Encoding")) {
        if (hasEncoding_) return fail(Error::InvalidHeader);
        hasEncoding_ = true;
        chunked_ = equalIgnoreCase(value, "chunked");
        if (!chunked_ && !equalIgnoreCase(value, "identity")) return fail(Error::UnsupportedEncoding);
      }
    }
    if (chunked_ && hasLength_) return fail(Error::InvalidHeader);
    if (options_.requireContentLength && (!hasLength_ || contentLength_ == 0))
      return fail(Error::InvalidHeader);
    return true;
  }
  bool bodyByte() {
    char c;
    const int n = byte(c);
    if (n != 1) { if (!n) fail(Error::Truncated); return false; }
    if (result_.payload.length() >= options_.maxBodyBytes) return fail(Error::BodyTooLarge);
    if (!append(result_.payload, c)) return fail(Error::NoMemory);
    return true;
  }
  bool identity() {
    if (hasLength_) {
      while (result_.payload.length() < contentLength_) if (!bodyByte()) return false;
      return true;
    }
    for (;;) {
      char c;
      const int n = byte(c);
      if (n == 0) return true;
      if (n < 0) return false;
      if (result_.payload.length() >= options_.maxBodyBytes) return fail(Error::BodyTooLarge);
      if (!append(result_.payload, c)) return fail(Error::NoMemory);
    }
  }
  bool chunks() {
    char text[128];
    for (;;) {
      if (!line(text, sizeof(text))) return false;
      size_t size = 0, pos = 0;
      for (; text[pos] && text[pos] != ';'; ++pos) {
        const char c = text[pos];
        const int n = c >= '0' && c <= '9' ? c - '0' :
          c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (n < 0) return fail(Error::InvalidBody);
        if (size_t(n) > options_.maxBodyBytes || size > (options_.maxBodyBytes - size_t(n)) / 16)
          return fail(Error::BodyTooLarge);
        size = size * 16 + size_t(n);
      }
      if (!pos) return fail(Error::InvalidBody);
      if (size > options_.maxBodyBytes - result_.payload.length()) return fail(Error::BodyTooLarge);
      if (!size) {
        do { if (!line(text, sizeof(text))) return false; } while (*text);
        return true;
      }
      for (size_t i = 0; i < size; ++i) if (!bodyByte()) return false;
      char cr, lf;
      if (byte(cr) != 1 || byte(lf) != 1) {
        if (result_.error == Error::None) fail(Error::Truncated);
        return false;
      }
      if (cr != '\r' || lf != '\n') return fail(Error::InvalidBody);
    }
  }
  Result finish() {
    const Phase phase = result_.phase;
    const bool success = result_.error == Error::None;
    const uint32_t phaseElapsed = uint32_t(io_.now() - phaseStarted_);
    if (!success) {
      switch (phase) {
        case Phase::Connect: result_.timings.connectMs = phaseElapsed; break;
        case Phase::Write: result_.timings.writeMs = phaseElapsed; break;
        case Phase::Header: result_.timings.headerMs = phaseElapsed; break;
        case Phase::Body: result_.timings.bodyMs = phaseElapsed; break;
        default: break;
      }
    }
    enter(Phase::Close);
    const uint32_t closeStarted = io_.now();
    io_.close();
    result_.timings.closeMs = uint32_t(io_.now() - closeStarted);
    if (success) check();
    result_.timings.totalMs = elapsed();
    enter(Phase::Done);
    result_.phase = result_.error == Error::None ? Phase::Done : (success ? Phase::Close : phase);
    if (result_.error != Error::None) result_.payload = "";
    return static_cast<Result &&>(result_);
  }
};

class SocketIo final : public detail::Io {
 public:
  ~SocketIo() override { close(); }
  uint32_t now() override {
#ifdef HAS2_BT_HOST
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#else
    return millis();
#endif
  }
  Connected beginConnect(const char *host, uint16_t port) override {
    sockaddr_in addr{};
    addr.sin_family = AF_INET; addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { error_ = EINVAL; return Connected::Failed; }
    fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd_ < 0) { error_ = errno; return Connected::Failed; }
    if (fd_ >= FD_SETSIZE) { error_ = EMFILE; return Connected::Failed; }
    const int flags = fcntl(fd_, F_GETFL, 0);
    if (flags < 0 || fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) { error_ = errno; return Connected::Failed; }
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    if (setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0) { error_ = errno; return Connected::Failed; }
#endif
    if (::connect(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) return Connected::Ready;
    if (errno == EINPROGRESS || errno == EALREADY || errno == EINTR) return Connected::Pending;
    error_ = errno; return Connected::Failed;
  }
  Connected pollConnect() override {
    fd_set writeSet, errorSet;
    FD_ZERO(&writeSet); FD_ZERO(&errorSet); FD_SET(fd_, &writeSet); FD_SET(fd_, &errorSet);
    timeval timeout{};
    const int n = select(fd_ + 1, nullptr, &writeSet, &errorSet, &timeout);
    if (n < 0) { if (errno == EINTR) return Connected::Pending; error_ = errno; return Connected::Failed; }
    if (!n) return Connected::Pending;
    int value = 0; socklen_t length = sizeof(value);
    if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &value, &length) < 0) { error_ = errno; return Connected::Failed; }
    if (value) { error_ = value; return Connected::Failed; }
    return Connected::Ready;
  }
  int write(const uint8_t *data, size_t length) override {
    int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    const int n = ::send(fd_, data, length, flags);
    if (n >= 0) return n ? n : -1;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return -1;
    error_ = errno; return -2;
  }
  int read(uint8_t *data, size_t length) override {
    const int n = ::recv(fd_, data, length, MSG_DONTWAIT);
    if (n >= 0) return n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return -1;
    error_ = errno; return -2;
  }
  int lastError() const override { return error_; }
  void idle() override {
#ifdef HAS2_BT_HOST
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
#else
    delay(1);
#endif
  }
  void close() override { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
 private:
  int fd_ = -1, error_ = 0;
};
} // namespace

Result detail::run(Io &io, const char *host, uint16_t port, const char *path, const Options &options) {
  return Engine(io, options).execute(host, port, path);
}
Result get(const char *host, uint16_t port, const char *path, const Options &options) {
  SocketIo io;
  return detail::run(io, host, port, path, options);
}
const char *errorName(Error error) {
  switch (error) {
    case Error::None: return "ok";
    case Error::InvalidRequest: return "invalid_request";
    case Error::Connect: return "connect";
    case Error::Write: return "write";
    case Error::Read: return "read";
    case Error::Deadline: return "deadline";
    case Error::HeaderTooLarge: return "header_too_large";
    case Error::InvalidHeader: return "invalid_header";
    case Error::UnsupportedEncoding: return "unsupported_encoding";
    case Error::BodyTooLarge: return "body_too_large";
    case Error::Truncated: return "truncated";
    case Error::InvalidBody: return "invalid_body";
    case Error::NoMemory: return "no_memory";
  }
  return "unknown";
}
const char *phaseName(Phase phase) {
  switch (phase) {
    case Phase::Connect: return "connect"; case Phase::Write: return "write";
    case Phase::Header: return "header"; case Phase::Body: return "body";
    case Phase::Close: return "close"; case Phase::Done: return "done";
  }
  return "unknown";
}
} // namespace Has2BoundedTransport
