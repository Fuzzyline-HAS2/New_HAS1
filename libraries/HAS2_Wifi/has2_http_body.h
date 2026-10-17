#ifndef HAS2_HTTP_BODY_H
#define HAS2_HTTP_BODY_H

#include <stddef.h>
#include <stdint.h>
#include <type_traits>

namespace Has2HttpBody {
enum class Result { Ok, Timeout, Truncated, ConnectionError, TooLarge, InvalidFraming, NoMemory };
static const uint32_t DeadlineMs = 1500;
static const size_t MaxBytes = 16 * 1024;

// Stream.read() is called only after available(): no Stream timeout loop.
// Clock supplies now()/idle(); unsigned subtraction tolerates millis rollover.
template<class Stream, class Buffer, class Clock>
class Reader {
 public:
  Reader(Stream *stream, Buffer &out, Clock &clock)
      : stream_(stream), out_(out), clock_(clock), started_(clock.now()) {}

  Result read(int length, bool chunked) {
    out_ = "";
    Result result = chunked ? chunks() : identity(length);
    if (result != Result::Ok) out_ = ""; // Never expose a partial JSON prefix.
    return result;
  }

 private:
  Stream *stream_;
  Buffer &out_;
  Clock &clock_;
  uint32_t started_;
  size_t wireBytes_ = 0;
  Result failure_ = Result::InvalidFraming;

  bool byte(char &value) {
    for (;;) {
      if (uint32_t(clock_.now() - started_) >= DeadlineMs) {
        failure_ = Result::Timeout;
        return false;
      }
      if (!stream_) { failure_ = Result::Truncated; return false; }
      if (stream_->available() > 0) {
        int received = stream_->read();
        if (received >= 0) {
          if (++wireBytes_ > MaxBytes + 4096) {
            failure_ = Result::TooLarge;
            return false;
          }
          value = static_cast<char>(received);
          return true;
        }
      }
      const int peer = clock_.peerState(stream_);
      if (peer != 0) {
        failure_ = peer > 0 ? Result::Truncated : Result::ConnectionError;
        return false;
      }
      clock_.idle();
    }
  }

  bool append(char value) {
    if (out_.length() >= MaxBytes) { failure_ = Result::TooLarge; return false; }
    if (!out_.concat(value)) { failure_ = Result::NoMemory; return false; }
    return true;
  }

  Result identity(int length) {
    if (length < -1) return Result::InvalidFraming;
    if (length > static_cast<int>(MaxBytes)) return Result::TooLarge;
    if (length == 0) return Result::Ok;
    if (!stream_) return length < 0 ? Result::Ok : Result::Truncated;
    while (length < 0 || out_.length() < static_cast<size_t>(length)) {
      char value;
      if (!byte(value)) {
        // EOF is the delimiter only for responses with no Content-Length.
        return length < 0 && failure_ == Result::Truncated ? Result::Ok : failure_;
      }
      if (!append(value)) return failure_;
    }
    return Result::Ok;
  }

  bool line(char *buffer, size_t capacity) {
    size_t used = 0;
    for (;;) {
      char value;
      if (!byte(value)) return false;
      if (value == '\r') {
        if (!byte(value)) return false;
        if (value != '\n') { failure_ = Result::InvalidFraming; return false; }
        buffer[used] = 0;
        return true;
      }
      if (value == '\n' || value == 0 || used + 1 >= capacity) {
        failure_ = Result::InvalidFraming;
        return false;
      }
      buffer[used++] = value;
    }
  }

  Result chunks() {
    for (;;) {
      char header[128];
      if (!line(header, sizeof(header))) return failure_;
      size_t size = 0;
      size_t pos = 0;
      for (; header[pos] && header[pos] != ';'; ++pos) {
        char c = header[pos];
        int digit = c >= '0' && c <= '9' ? c - '0' :
                    c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                    c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return Result::InvalidFraming;
        if (size > (MaxBytes - static_cast<size_t>(digit)) / 16)
          return Result::TooLarge;
        size = size * 16 + static_cast<size_t>(digit);
      }
      if (!pos) return Result::InvalidFraming;
      if (size > MaxBytes - out_.length()) return Result::TooLarge;
      if (!size) {
        // Consume trailers and their terminating CRLF, under the same budget.
        do { if (!line(header, sizeof(header))) return failure_; } while (header[0]);
        return Result::Ok;
      }
      for (size_t i = 0; i < size; ++i) {
        char value;
        if (!byte(value) || !append(value)) return failure_;
      }
      char cr, lf;
      if (!byte(cr) || !byte(lf)) return failure_;
      if (cr != '\r' || lf != '\n') return Result::InvalidFraming;
    }
  }
};

// Stop first: HTTPClient::end() otherwise drains unread bytes in clear(),
// which can itself run indefinitely against a continuously streaming peer.
template<class Http> void closeResponse(Http &http) {
  auto *stream = http.getStreamPtr();
  if (stream) stream->stop();
  http.end();
}

// Framing validation deliberately avoids HTTPClient's permissive toInt().
// HTTP must have collected Transfer-Encoding and Content-Length before GET().
template<class Http, class Buffer, class Clock>
Result readResponse(Http &http, Buffer &out, Clock &clock) {
  out = "";
  const auto encoding = http.header("Transfer-Encoding");
  const auto lengthText = http.header("Content-Length");
  bool chunked = encoding.equalsIgnoreCase("chunked");
  if (encoding.length() && !chunked && !encoding.equalsIgnoreCase("identity"))
    return Result::InvalidFraming;
  if (chunked && lengthText.length()) return Result::InvalidFraming;
  int length = -1;
  if (lengthText.length()) {
    length = 0;
    for (size_t i = 0; i < lengthText.length(); ++i) {
      char c = lengthText[i];
      if (c < '0' || c > '9') return Result::InvalidFraming;
      if (length > (static_cast<int>(MaxBytes) - (c - '0')) / 10)
        return Result::TooLarge;
      length = length * 10 + (c - '0');
    }
  } else if (http.hasHeader("Content-Length")) {
    return Result::InvalidFraming;
  }
  auto *stream = http.getStreamPtr();
  Reader<typename std::remove_reference<decltype(*stream)>::type, Buffer, Clock> reader(stream, out, clock);
  return reader.read(length, chunked);
}
} // namespace Has2HttpBody
#endif
