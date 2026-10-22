#pragma once
#include <HTTPClient.h>
#include "HAS2HttpResult.h"

// All loops use elapsed subtraction, including across millis() rollover.
class HAS2DeadlineClient : public NetworkClient {
 public:
  HAS2DeadlineClient(HAS2HttpResult &result, HAS2HttpProgressHook hook, void *context);
  int connect(const char *host, uint16_t port, int32_t timeout) override;
  size_t write(const uint8_t *data, size_t length) override;
  size_t write(uint8_t data) override { return write(&data, 1); }
  int read(uint8_t *data, size_t length) override;
  int read() override;
  size_t readBytes(char *data, size_t length) override;
  using NetworkClient::readBytes;
  int available() override;
  uint8_t connected() override;
  bool alive();
  bool line(String &output, size_t limit);
  void phase(HAS2HttpPhase value);
  void fail(HAS2HttpError value);
  uint32_t elapsed() const;
 private:
  HAS2HttpResult &result_;
  HAS2HttpProgressHook hook_;
  void *context_;
  uint32_t started_, progress_, notified_;
  uint32_t wireBytes_ = 0;
  void progress();
};

class HAS2BoundedHTTP : public HTTPClient {
 public:
  explicit HAS2BoundedHTTP(HAS2DeadlineClient &socket) : socket_(socket) {}
  int get();
  int writeToStream(Stream *stream);
 private:
  HAS2DeadlineClient &socket_;
  int handleHeaderResponse();
  int writeToStreamDataBlock(Stream *stream, int length);
  bool line(String &value, size_t limit) { return socket_.line(value, limit); }
};

bool has2BoundedGet(const String &url, String &payload, HAS2HttpResult &result,
                    HAS2HttpProgressHook hook, void *context);
