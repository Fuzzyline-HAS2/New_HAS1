#include "HAS2BoundedHTTP.h"
#include <errno.h>
#include <sys/socket.h>
#include <stdlib.h>

HAS2DeadlineClient::HAS2DeadlineClient(HAS2HttpResult &result, HAS2HttpProgressHook hook, void *context)
    : result_(result), hook_(hook), context_(context), started_(millis()),
      progress_(started_), notified_(started_) { phase(HAS2HttpPhase::Connect); }
uint32_t HAS2DeadlineClient::elapsed() const { return uint32_t(millis() - started_); }
void HAS2DeadlineClient::fail(HAS2HttpError value) {
  if (result_.error == HAS2HttpError::None) result_.error = value;
  stop();
}
bool HAS2DeadlineClient::alive() {
  if (result_.error != HAS2HttpError::None) return false;
  if (elapsed() >= 5000 || uint32_t(millis() - progress_) >= 1500) {
    fail(HAS2HttpError::Transport); return false;
  }
  return true;
}
void HAS2DeadlineClient::phase(HAS2HttpPhase value) {
  result_.phase = value;
  if (hook_) hook_(value, result_.bytes, context_);
}
void HAS2DeadlineClient::progress() {
  progress_ = millis();
  if (hook_ && uint32_t(progress_ - notified_) >= 100) {
    notified_ = progress_; hook_(result_.phase, result_.bytes, context_);
  }
}
int HAS2DeadlineClient::connect(const char *host, uint16_t port, int32_t) {
  IPAddress ip;
  // No unbounded hostByName or TLS handshake in this local HTTP lane.
  if (!ip.fromString(host) || !alive()) { fail(HAS2HttpError::Transport); return 0; }
  const uint32_t remaining = 5000 - elapsed();
  const uint32_t idle = 1500 - uint32_t(millis() - progress_);
  const int ok = NetworkClient::connect(ip, port, remaining < idle ? remaining : idle);
  if (!alive() || !ok) { fail(HAS2HttpError::Transport); return 0; }
  progress(); return 1;
}
size_t HAS2DeadlineClient::write(const uint8_t *data, size_t length) {
  phase(HAS2HttpPhase::Write);
  size_t done = 0;
  while (done < length && alive()) {
    const int n = ::send(fd(), data + done, length - done, MSG_DONTWAIT);
    if (n > 0) { done += n; progress(); }
    else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      fail(HAS2HttpError::Transport); break;
    } else delay(1);
  }
  return done;
}
int HAS2DeadlineClient::read(uint8_t *data, size_t length) {
  if (!alive() || fd() < 0) return -1;
  const int n = ::recv(fd(), data, length, MSG_DONTWAIT);
  if (n > 0) {
    wireBytes_ += n;
    if (wireBytes_ > 16384) { fail(HAS2HttpError::TooLarge); return -1; }
    progress(); return n;
  }
  if (n == 0) { stop(); return -1; }
  if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
    fail(HAS2HttpError::Transport); return -1;
  }
  return 0;
}
int HAS2DeadlineClient::read() {
  uint8_t value;
  return read(&value, 1) == 1 ? value : -1;
}
size_t HAS2DeadlineClient::readBytes(char *data, size_t length) {
  size_t done = 0;
  while (done < length && alive()) {
    const int n = read(reinterpret_cast<uint8_t *>(data) + done, length - done);
    if (n < 0) break;
    if (n > 0) done += n; else delay(1);
  }
  return done;
}
int HAS2DeadlineClient::available() {
  if (!alive() || fd() < 0) return 0;
  uint8_t preview[512];
  const int n = ::recv(fd(), preview, sizeof(preview), MSG_PEEK | MSG_DONTWAIT);
  if (n > 0) return n;
  if (n == 0) stop();
  else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) fail(HAS2HttpError::Transport);
  return 0;
}
uint8_t HAS2DeadlineClient::connected() {
  available();
  return fd() >= 0 && result_.error == HAS2HttpError::None;
}
bool HAS2DeadlineClient::line(String &output, size_t limit) {
  output = "";
  while (alive()) {
    char c;
    if (readBytes(&c, 1) != 1) { fail(HAS2HttpError::Transport); return false; }
    if (c == '\n') {
      if (!output.endsWith("\r")) { fail(HAS2HttpError::Framing); return false; }
      output.remove(output.length() - 1); return true;
    }
    if (output.length() >= limit) { fail(HAS2HttpError::TooLarge); return false; }
    output += c;
  }
  return false;
}
int HAS2BoundedHTTP::get() {
  if (!connect()) return HTTPC_ERROR_CONNECTION_REFUSED;
  if (!sendHeader("GET")) return HTTPC_ERROR_SEND_HEADER_FAILED;
  socket_.phase(HAS2HttpPhase::Headers);
  return handleHeaderResponse();
}

// Uses the core parser's status/header and chunk flow; changes are scoped here.
#include "HAS2BoundedHTTP_parser.inc"

namespace {
class Body : public Stream {
 public:
  String value;
  bool tooLarge = false;
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *data, size_t length) override {
    if (length > 8192 - value.length()) { tooLarge = true; return 0; }
    const size_t before = value.length();
    if (!value.concat(reinterpret_cast<const char *>(data), length)) return 0;
    return value.length() - before;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
};
uint32_t retryAfter(const String &text) {
  if (!text.length()) return 0;
  uint32_t seconds = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    if (text[i] < '0' || text[i] > '9') return 0; // HTTP-date requires a trusted clock.
    seconds = seconds > 120 ? 121 : seconds * 10 + (text[i] - '0');
  }
  return seconds > 120 ? 120000 : seconds * 1000;
}
}
bool has2BoundedGet(const String &url, String &payload, HAS2HttpResult &result,
                    HAS2HttpProgressHook hook, void *context) {
  result = HAS2HttpResult{};
  HAS2DeadlineClient socket(result, hook, context);
  HAS2BoundedHTTP http(socket);
  Body body;
  bool ok = false;
  if (!url.startsWith("http://") || !http.begin(socket, url)) {
    socket.fail(HAS2HttpError::Transport);
  } else {
    http.setReuse(false);
    http.setTimeout(1500);
    http.setConnectTimeout(1500);
    const char *headers[] = {"Retry-After"};
    http.collectHeaders(headers, 1);
    result.status = http.get();
    if (result.status > 0 && result.status != 200) {
      // Valid complete error headers prove a server response. We do not need
      // its body, and a stalled overload page must not look like broken Wi-Fi.
      if (result.status == 429 || result.status == 503) result.retryAfterMs = retryAfter(http.header("Retry-After"));
      result.error = HAS2HttpError::HttpStatus;
    } else if (result.status > 0) {
      socket.phase(HAS2HttpPhase::Body);
      if (http.getSize() > 8192) socket.fail(HAS2HttpError::TooLarge);
      else {
        const int bytes = http.writeToStream(&body);
        result.bytes = body.value.length();
        if (body.tooLarge) socket.fail(HAS2HttpError::TooLarge);
        else if (bytes < 0 && result.error == HAS2HttpError::None) socket.fail(HAS2HttpError::Framing);
        else if (result.error == HAS2HttpError::None) {
          if (result.status == 429 || result.status == 503) result.retryAfterMs = retryAfter(http.header("Retry-After"));
          if (result.status == 200) { payload = body.value; ok = true; }
          else result.error = HAS2HttpError::HttpStatus;
        }
      }
    } else if (result.error == HAS2HttpError::None) socket.fail(HAS2HttpError::Transport);
  }
  // Stop before SDK end(), which otherwise drains unread bytes without a deadline.
  socket.stop(); http.end();
  result.elapsedMs = socket.elapsed();
  // Preserve failing phase for diagnostics; Complete is a hook event only.

  return ok;
}
