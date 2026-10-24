#pragma once
#include "BlackBoxRecord.h"
namespace has1blackbox {
struct Config {
  const char *device;
  const char *firmware;
  const char *brokerUri;
  bool enabled = true;
  const char *username = "";
  const char *password = "";
};
enum class FlushResult { Persisted, Failed, TimedOut, Unavailable };
struct Diagnostics {
  uint32_t accepted = 0, persisted = 0, dropped = 0, flashErrors = 0,
           uploadFailures = 0, uploaded = 0, queueDepth = 0;
  bool ready = false, paused = false;
  uint32_t eraseCalls = 0, maxEraseUs = 0, overwritten = 0;
};
class BlackBox {
public:
  bool begin(const Config &);
  uint32_t log(EventType, int32_t a = 0, int32_t b = 0, int32_t c = 0,
               int32_t d = 0, const char *text = "");
  void setUploadAllowed(bool);
  void setPaused(bool);
  bool tryBeginLedOutput();
  void endLedOutput();
  bool ready() const;
  FlushResult flushThrough(uint32_t ticket, uint32_t timeoutMs);
  Diagnostics diagnostics() const;
};
extern BlackBox blackbox;
} // namespace has1blackbox
