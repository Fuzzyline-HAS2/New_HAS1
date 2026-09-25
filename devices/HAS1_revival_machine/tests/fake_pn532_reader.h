#pragma once
#include <cassert>
#include <cstring>
#include <deque>
#include <vector>
#include "pn532_transport.h"

// Semantic fake at the production adapter boundary. Byte-level behavior is
// independently covered by pn532_transport_tests.cpp against the real adapter.
struct FakePn532Reader {
  std::deque<bool> uidResults, readResults, ackResults;
  std::deque<Pn532Result> targetResults, pageResults, versionResults, samResults;
  std::deque<uint32_t> targetDurationsUs, pageDurationsUs, configDurationsUs, samDurationsUs, versionDurationsUs;
  std::vector<std::vector<unsigned int>> operations;
  std::vector<std::vector<uint8_t>> commands;
  uint8_t payload[4] = {'G', '9', 'P', '3'};
  unsigned uidCalls = 0, readCalls = 0, retryCalls = 0, versionCalls = 0, abortCalls = 0;
  uint16_t lastTimeout = 0;
  uint8_t lastRetries = 0, uidLengthResult = 7;
  Pn532Fault fault = Pn532Fault::None;
  uint8_t status = 0, tagStatus = 0;
  bool pinsOk = true;
  template <typename T> static T Pop(std::deque<T>& values, T fallback) {
    if (values.empty()) return fallback;
    T value = values.front(); values.pop_front(); return value;
  }
  Pn532Result Step(const Pn532Deadline& deadline, uint32_t duration, Pn532Result result) {
    if (deadline.expired()) return Pn532Result::Deadline;
    uint32_t availableUs = deadline.remaining() * 1000;
    fakeTimeUs += duration < availableUs ? duration : availableUs;
    if (duration >= availableUs) result = Pn532Result::Deadline;
    fault = result == Pn532Result::TransportFault ? Pn532Fault::StatusFlags :
            result == Pn532Result::Deadline ? Pn532Fault::Budget : Pn532Fault::None;
    status = result == Pn532Result::TransportFault ? 5 : 1;
    tagStatus = result == Pn532Result::TagError ? 1 : 0;
    return result;
  }
  bool beginPins() { operations.push_back({4}); return pinsOk; }
  Pn532Result wake(const Pn532Deadline& deadline) {
    operations.push_back({5}); return Step(deadline, 20, Pn532Result::Ok);
  }
  Pn532Result abort(const Pn532Deadline& deadline) {
    ++abortCalls; operations.push_back({6}); return Step(deadline, 20, Pn532Result::Ok);
  }
  Pn532Result getFirmwareVersion(uint32_t& version, const Pn532Deadline& deadline) {
    ++versionCalls; operations.push_back({7});
    auto result = Step(deadline.limited(100), Pop(versionDurationsUs, uint32_t(20)), Pop(versionResults, Pn532Result::Ok));
    version = result == Pn532Result::Ok ? 0x32010607 : 0; return result;
  }
  Pn532Result configureSam(const Pn532Deadline& deadline) {
    operations.push_back({8}); return Step(deadline.limited(100), Pop(samDurationsUs, uint32_t(20)), Pop(samResults, Pn532Result::Ok));
  }
  Pn532Result Config(std::vector<uint8_t> bytes, const Pn532Deadline& deadline) {
    commands.push_back(bytes);
    const auto bounded = deadline.limited(100);
    std::vector<unsigned int> operation{1, bounded.remaining()};
    operation.insert(operation.end(), bytes.begin(), bytes.end()); operations.push_back(operation);
    return Step(bounded, Pop(configDurationsUs, uint32_t(30)),
                Pop(ackResults, true) ? Pn532Result::Ok : Pn532Result::TransportFault);
  }
  Pn532Result setRetries(uint8_t retries, const Pn532Deadline& deadline) {
    ++retryCalls; lastRetries = retries;
    return Config({0x32, 5, 0xFF, 1, retries}, deadline);
  }
  Pn532Result setGain(uint8_t config, const Pn532Deadline& deadline) {
    return Config({0x32, 0x0A, config, 0xF4, 0x3F, 0x11, 0x4D, 0x85,
                   0x61, 0x6F, 0x26, 0x62, 0x87}, deadline);
  }
  Pn532Result setRfField(bool enabled, const Pn532Deadline& deadline) {
    return Config({0x32, 1, static_cast<uint8_t>(enabled)}, deadline);
  }
  Pn532Result readTarget(uint8_t* uid, uint8_t& length, const Pn532Deadline& deadline) {
    const auto bounded = deadline.limited(250);
    ++uidCalls; lastTimeout = bounded.remaining();
    operations.push_back({2, 0, bounded.remaining()});
    auto fallback = Pop(uidResults, false) ? Pn532Result::Ok : Pn532Result::NoTarget;
    auto result = Step(bounded, Pop(targetDurationsUs, uint32_t(100)), Pop(targetResults, fallback));
    length = result == Pn532Result::Ok ? uidLengthResult : 0;
    assert(length <= 10);
    if (length) memcpy(uid, "1234567890", length);
    return result;
  }
  Pn532Result readPage7(uint8_t* data, const Pn532Deadline& deadline) {
    ++readCalls; operations.push_back({3, 7});
    auto fallback = Pop(readResults, true) ? Pn532Result::Ok : Pn532Result::TagError;
    auto result = Step(deadline.limited(100), Pop(pageDurationsUs, uint32_t(50)), Pop(pageResults, fallback));
    if (result == Pn532Result::Ok) memcpy(data, payload, 4);
    return result;
  }
  const char* phaseName() const { return "response_wait"; }
  const char* faultName() const { return fault == Pn532Fault::None ? "none" : fault == Pn532Fault::StatusFlags ? "status_flags" : "budget"; }
  Pn532Fault lastFault() const { return fault; }
  Pn532Phase lastPhase() const { return Pn532Phase::ResponseWait; }
  uint8_t lastStatus() const { return status; }
  uint8_t lastTagStatus() const { return tagStatus; }
};
