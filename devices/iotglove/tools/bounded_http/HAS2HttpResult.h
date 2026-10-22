#pragma once
#include <stdint.h>
enum class HAS2HttpError : uint8_t { None, Transport, HttpStatus, Framing, TooLarge, Json };
enum class HAS2HttpPhase : uint8_t { Idle, Connect, Write, Headers, Body, Complete };
struct HAS2HttpResult {
  HAS2HttpError error = HAS2HttpError::None;
  HAS2HttpPhase phase = HAS2HttpPhase::Idle;
  int status = 0;
  uint32_t elapsedMs = 0, bytes = 0, retryAfterMs = 0;
};
using HAS2HttpProgressHook = void (*)(HAS2HttpPhase, uint32_t, void *);

class String;
using HAS2BoundedTransport = bool (*)(const String &, String &, HAS2HttpResult &,
                                     HAS2HttpProgressHook, void *);
bool has2BoundedGet(const String &url, String &payload, HAS2HttpResult &result,
                    HAS2HttpProgressHook hook, void *context);
