#include "BeaconState.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Has1BleBeacon;

#define REQUIRE(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (false)

constexpr uint16_t RESET = 0x0c03;
constexpr uint16_t PARAMETERS = 0x2006;
constexpr uint16_t ADVERTISING = 0x2008;
constexpr uint16_t SCAN_RESPONSE = 0x2009;
constexpr uint16_t ENABLE = 0x200a;

// Independent HCI/AD wire parser: inspect packets emitted by the real state
// machine, without rebuilding their payload using implementation helpers.
std::string advertisedName(const Command& command) {
  REQUIRE(command.opcode == ADVERTISING || command.opcode == SCAN_RESPONSE);
  REQUIRE(command.size == 36);
  REQUIRE(command.data[4] <= 31);
  const size_t end = 5 + command.data[4];
  std::string name;
  bool power = false;
  bool flags = false;
  for (size_t pos = 5; pos < end;) {
    const size_t length = command.data[pos];
    REQUIRE(length >= 1);
    REQUIRE(pos + length + 1 <= end);
    const auto type = command.data[pos + 1];
    if (type == 0x09) {
      REQUIRE(name.empty());
      name.assign(reinterpret_cast<const char*>(command.data + pos + 2), length - 1);
    } else if (type == 0x01) {
      REQUIRE(length == 2 && command.data[pos + 2] == 0x06);
      flags = true;
    } else if (type == 0x0a) {
      REQUIRE(length == 2 && command.data[pos + 2] == 3);
      power = true;
    }
    pos += length + 1;
  }
  REQUIRE(name.compare(0, 5, "HAS3:") == 0);
  REQUIRE(power);
  REQUIRE(flags == (command.opcode == ADVERTISING));
  for (size_t pos = end; pos < sizeof(command.data); ++pos) REQUIRE(command.data[pos] == 0);
  return name;
}

struct Fixture {
  StateMachine machine;
  uint32_t now = 1000;
  std::vector<Command> sent;
  std::vector<std::string> enabledNames;
  std::string adv;
  std::string scan;

  explicit Fixture(const char* name = "duct_1") {
    machine.setDeviceName(name); // The boot snapshot precedes controller startup.
    machine.start(true);
  }

  const Command* poll(bool allow = true, bool available = true) {
    const bool outstanding = machine.diagnostics().awaitingCompletion;
    const Command* command = machine.poll(now, allow, available);
    if (command != nullptr) {
      REQUIRE(!outstanding);
      REQUIRE(command->size >= 4 && command->size <= sizeof(command->data));
      REQUIRE(command->data[0] == 1);
      REQUIRE(command->data[3] == command->size - 4);
      REQUIRE(command->opcode == (command->data[1] | (command->data[2] << 8)));
      REQUIRE(machine.diagnostics().awaitingCompletion);
      REQUIRE(machine.diagnostics().pendingOpcode == command->opcode);
      sent.push_back(*command);
    }
    return command;
  }

  Command next() {
    for (unsigned attempt = 0; attempt < 32; ++attempt, ++now) {
      if (const Command* command = poll()) return *command;
    }
    throw std::runtime_error("Expected an HCI command within bounded available polls");
  }

  void ack(const Command& command, uint8_t status = 0, uint32_t elapsed = 1) {
    now += elapsed;
    if (status == 0) {
      if (command.opcode == RESET) { adv.clear(); scan.clear(); }
      if (command.opcode == ADVERTISING) adv = advertisedName(command);
      if (command.opcode == SCAN_RESPONSE) scan = advertisedName(command);
      if (command.opcode == ENABLE && command.data[4] == 1) {
        REQUIRE(!adv.empty());
        REQUIRE(adv == scan); // A receiving glove must never see mixed names.
        enabledNames.push_back(adv);
      }
    }
    machine.complete(command.opcode, status, now);
  }

  void running(const std::string& expected = "HAS3:duct_1") {
    for (unsigned attempt = 0; attempt < 128; ++attempt, ++now) {
      if (machine.diagnostics().step == Step::Running &&
          !enabledNames.empty() && enabledNames.back() == expected) return;
      if (const Command* command = poll()) ack(*command);
    }
    throw std::runtime_error("Beacon did not converge to " + expected);
  }
};

void normalSequence() {
  Fixture f;
  const uint16_t order[] = {RESET, PARAMETERS, ADVERTISING, SCAN_RESPONSE, ENABLE};
  for (const auto opcode : order) {
    const Command command = f.next();
    REQUIRE(command.opcode == opcode);
    REQUIRE(f.machine.diagnostics().step != Step::Running);
    REQUIRE(f.machine.diagnostics().radio != RadioState::On);
    for (unsigned repeat = 0; repeat < 5; ++repeat) REQUIRE(f.poll() == nullptr);
    if (opcode == RESET) REQUIRE(command.size == 4);
    if (opcode == PARAMETERS) {
      REQUIRE(command.size == 19);
      REQUIRE((command.data[4] | (command.data[5] << 8)) == 400); // 400 * 0.625 ms
      REQUIRE((command.data[6] | (command.data[7] << 8)) == 400);
      REQUIRE(command.data[8] == 2); // Scannable, non-connectable.
      REQUIRE(command.data[17] == 7); // All three advertising channels.
    }
    if (opcode == ADVERTISING || opcode == SCAN_RESPONSE)
      REQUIRE(advertisedName(command) == "HAS3:duct_1");
    if (opcode == ENABLE) REQUIRE(command.data[4] == 1);
    f.ack(command);
  }
  REQUIRE(f.machine.diagnostics().step == Step::Running);
  REQUIRE(f.machine.diagnostics().radio == RadioState::On);
  REQUIRE(f.machine.diagnostics().sentCommands == 5);
  for (unsigned i = 0; i < 10000; ++i) { f.now += 37; REQUIRE(f.poll() == nullptr); }
  REQUIRE(f.machine.diagnostics().sentCommands == 5);
}

void copiedAndBoundedNames() {
  Fixture f;
  char name[] = "Ab_12-xyz";
  f.machine.setDeviceName(name);
  std::memset(name, 'q', sizeof(name) - 1);
  f.running("HAS3:Ab_12-xyz");
  f.machine.setDeviceName("abcdefghijklmnopqr"); // 18 bytes, maximum AD payload.
  f.running("HAS3:abcdefghijklmnopqr");
  for (const auto& command : f.sent) {
    if (command.opcode == ADVERTISING && advertisedName(command) == "HAS3:abcdefghijklmnopqr")
      REQUIRE(command.data[4] == 31);
  }
  // A valid maximum-length C string ends exactly at the bound: ASan checks overreads.
  char* exact = new char[19];
  std::memcpy(exact, "ABCDEFGHIJKLMNOPQR", 19);
  f.machine.setDeviceName(exact);
  delete[] exact;
  f.running("HAS3:ABCDEFGHIJKLMNOPQR");
}

void invalidNamesStop() {
  const char* invalid[] = {"abcdefghijklmnopqrs", "\xc3\xa9", "", nullptr, "bad name", "bad:name"};
  for (const auto name : invalid) {
    Fixture f;
    f.running();
    f.machine.setDeviceName(name);
    const Command stop = f.next();
    REQUIRE(stop.opcode == ENABLE && stop.data[4] == 0);
    f.ack(stop);
    REQUIRE(f.machine.diagnostics().radio == RadioState::Off);
    REQUIRE(f.machine.diagnostics().lastError == Error::InvalidDeviceName);
    for (unsigned i = 0; i < 100; ++i) { f.now += 100; REQUIRE(f.poll() == nullptr); }
    f.machine.setDeviceName("valid_again");
    f.running("HAS3:valid_again");
  }
}

void latestNameAtEveryStage() {
  // Change input with every command type outstanding, including Disable.
  for (unsigned stage = 0; stage < 6; ++stage) {
    Fixture f;
    Command pending{};
    if (stage == 5) {
      f.running();
      f.machine.setDeviceName("intermediate");
      pending = f.next();
      REQUIRE(pending.opcode == ENABLE && pending.data[4] == 0);
    } else {
      for (unsigned index = 0; index <= stage; ++index) {
        pending = f.next();
        if (index != stage) f.ack(pending);
      }
    }
    f.machine.setDeviceName("discarded");
    f.machine.setDeviceName("latest_name");
    REQUIRE(f.poll() == nullptr); // Must retain the in-flight command.
    f.ack(pending);
    f.running("HAS3:latest_name");
    REQUIRE(std::find(f.enabledNames.begin(), f.enabledNames.end(), "HAS3:discarded") == f.enabledNames.end());
    const size_t count = f.sent.size();
    f.machine.setDeviceName("latest_name");
    for (unsigned i = 0; i < 100; ++i) { ++f.now; REQUIRE(f.poll() == nullptr); }
    REQUIRE(f.sent.size() == count);
  }
}

void clearNameAtEveryStage() {
  for (unsigned stage = 0; stage < 5; ++stage) {
    Fixture f;
    Command pending{};
    for (unsigned index = 0; index <= stage; ++index) {
      pending = f.next();
      if (index != stage) f.ack(pending);
    }
    f.machine.setDeviceName("");
    f.ack(pending);
    for (unsigned attempt = 0; attempt < 32; ++attempt, ++f.now) {
      if (const Command* command = f.poll()) {
        REQUIRE(command->opcode == RESET || (command->opcode == ENABLE && command->data[4] == 0));
        f.ack(*command);
      }
    }
    REQUIRE(f.machine.diagnostics().radio == RadioState::Off);
    REQUIRE(!f.machine.diagnostics().awaitingCompletion);
  }
}

void busyGate() {
  Fixture f;
  for (unsigned i = 0; i < 1000; ++i) { ++f.now; REQUIRE(f.poll(false, false) == nullptr); }
  REQUIRE(f.machine.diagnostics().failures == 0);
  const Command reset = f.next();
  f.ack(reset);
  for (unsigned i = 0; i < 1000; ++i) { ++f.now; REQUIRE(f.poll(false) == nullptr); }
  REQUIRE(!f.machine.diagnostics().awaitingCompletion);
  f.running();
  const size_t runningCommands = f.sent.size();
  for (unsigned i = 0; i < 1000; ++i) { ++f.now; REQUIRE(f.poll(i % 2 == 0) == nullptr); }
  REQUIRE(f.sent.size() == runningCommands);
  f.machine.setDeviceName("new_name");
  REQUIRE(f.poll(false) == nullptr);
  REQUIRE(f.machine.diagnostics().radio == RadioState::On);
  const Command disable = f.next();
  REQUIRE(disable.data[4] == 0);
  f.now += kCompletionTimeoutMs;
  REQUIRE(f.poll(false) == nullptr);
  REQUIRE(f.machine.diagnostics().completionTimedOut);
  REQUIRE(f.machine.diagnostics().failures == 1);
}

void sendReadyBackoff() {
  Fixture f;
  REQUIRE(f.poll(true, false) == nullptr);
  f.now += kSendReadyTimeoutMs - 1;
  REQUIRE(f.poll(true, false) == nullptr);
  REQUIRE(f.machine.diagnostics().failures == 0);
  ++f.now;
  REQUIRE(f.poll(true, false) == nullptr);
  REQUIRE(f.machine.diagnostics().failures == 1);
  REQUIRE(f.machine.diagnostics().lastError == Error::SendUnavailable);
  REQUIRE(f.machine.diagnostics().sentCommands == 0);
  const uint32_t failedAt = f.now;
  for (uint32_t elapsed = 0; elapsed < kRetryDelayMs; ++elapsed) {
    f.now = failedAt + elapsed;
    REQUIRE(f.poll() == nullptr);
  }
  f.now = failedAt + kRetryDelayMs;
  const Command retry = f.next();
  REQUIRE(retry.opcode == RESET);
  REQUIRE(f.machine.diagnostics().sentCommands == 1);
  REQUIRE(f.poll() == nullptr);
  f.ack(retry);
  f.running();
}

void eachRejectedCommand() {
  for (unsigned stage = 0; stage < 6; ++stage) {
    Fixture f;
    Command pending{};
    if (stage == 5) {
      f.running();
      f.machine.setDeviceName("after_reject");
      pending = f.next();
      REQUIRE(pending.opcode == ENABLE && pending.data[4] == 0);
    } else {
      for (unsigned index = 0; index <= stage; ++index) {
        pending = f.next();
        if (index != stage) f.ack(pending);
      }
    }
    f.ack(pending, 0x0c);
    REQUIRE(f.machine.diagnostics().failures == 1);
    REQUIRE(f.machine.diagnostics().lastError == Error::CommandRejected);
    REQUIRE(f.machine.diagnostics().hciStatus == 0x0c);
    REQUIRE(!f.machine.diagnostics().awaitingCompletion);
    const uint32_t rejectedAt = f.now;
    f.now = rejectedAt + kRetryDelayMs - 1;
    REQUIRE(f.poll() == nullptr);
    f.now = rejectedAt + kRetryDelayMs;
    const Command reset = f.next();
    REQUIRE(reset.opcode == RESET);
    f.ack(reset);
    f.running(stage == 5 ? "HAS3:after_reject" : "HAS3:duct_1");
  }
}

void missingCompletion() {
  Fixture f;
  const Command command = f.next();
  const uint32_t sentAt = f.now;
  f.now = sentAt + kCompletionTimeoutMs - 1;
  REQUIRE(f.poll() == nullptr);
  REQUIRE(f.machine.diagnostics().failures == 0);
  ++f.now;
  REQUIRE(f.poll() == nullptr);
  REQUIRE(f.machine.diagnostics().failures == 1);
  REQUIRE(f.machine.diagnostics().awaitingCompletion);
  REQUIRE(f.machine.diagnostics().pendingOpcode == command.opcode);
  for (unsigned i = 0; i < 10000; ++i) {
    f.now += 7;
    f.machine.setDeviceName(i % 2 == 0 ? "later_one" : "later_two");
    REQUIRE(f.poll() == nullptr);
  }
  REQUIRE(f.machine.diagnostics().failures == 1);
  REQUIRE(f.machine.diagnostics().sentCommands == 1);
  f.ack(command); // Only a real late final ACK releases opcode ownership.
  REQUIRE(f.machine.diagnostics().failures == 1);
  REQUIRE(!f.machine.diagnostics().awaitingCompletion);
  f.now += kRetryDelayMs - 1;
  REQUIRE(f.poll() == nullptr);
  ++f.now;
  const Command reset = f.next();
  REQUIRE(reset.opcode == RESET);
  f.ack(reset);
  f.running("HAS3:later_two");
}

void wrongOpcodeAndDelayedConsumption() {
  Fixture f;
  const Command command = f.next();
  const uint32_t sentAt = f.now;
  f.machine.complete(PARAMETERS, 0, sentAt + 1);
  REQUIRE(f.machine.diagnostics().awaitingCompletion);
  REQUIRE(f.machine.diagnostics().pendingOpcode == RESET);
  // Callback receives before the deadline; loop was busy for several seconds.
  // The adapter consumes this completion before checking timeout in poll().
  f.now += 5000;
  f.machine.complete(command.opcode, 0, sentAt + kCompletionTimeoutMs - 1);
  REQUIRE(f.machine.diagnostics().failures == 0);
  const Command next = f.next();
  REQUIRE(next.opcode == PARAMETERS);
  f.ack(next);
  f.running();
}

void timelyCompletionAfterProvisionalTimeout() {
  for (unsigned stage = 0; stage < 6; ++stage) {
    Fixture f;
    Command pending{};
    if (stage == 5) {
      f.running();
      f.machine.setDeviceName("after_busy");
      pending = f.next();
      REQUIRE(pending.opcode == ENABLE && pending.data[4] == 0);
    } else {
      for (unsigned index = 0; index <= stage; ++index) {
        pending = f.next();
        if (index != stage) f.ack(pending);
      }
    }
    const uint32_t sentAt = f.now;
    f.now += kCompletionTimeoutMs + 100;
    REQUIRE(f.poll(false) == nullptr);
    REQUIRE(f.machine.diagnostics().completionTimedOut);
    REQUIRE(f.machine.diagnostics().failures == 1);
    // Cross-core mailbox race: the timestamp proves reception was timely even
    // though poll provisionally noticed expiry before consuming the callback.
    f.machine.complete(pending.opcode, 0, sentAt + kCompletionTimeoutMs - 1);
    REQUIRE(!f.machine.diagnostics().completionTimedOut);
    REQUIRE(!f.machine.diagnostics().awaitingCompletion);
    REQUIRE(f.machine.diagnostics().failures == 0);
    REQUIRE(f.machine.diagnostics().lastError != Error::ResponseTimeout);
    REQUIRE(f.machine.diagnostics().step != Step::Backoff);
    if (stage == 4) {
      REQUIRE(f.machine.diagnostics().step == Step::Running);
      REQUIRE(f.machine.diagnostics().radio == RadioState::On);
    } else {
      REQUIRE(f.machine.diagnostics().radio == RadioState::Off);
    }
  }
}

void rollover() {
  Fixture f;
  f.now = UINT32_MAX - 20;
  REQUIRE(f.poll(true, false) == nullptr);
  f.now += kSendReadyTimeoutMs - 1;
  REQUIRE(f.poll(true, false) == nullptr);
  REQUIRE(f.machine.diagnostics().failures == 0);
  ++f.now;
  REQUIRE(f.poll(true, false) == nullptr);
  REQUIRE(f.machine.diagnostics().failures == 1);
  f.now += kRetryDelayMs - 1;
  REQUIRE(f.poll() == nullptr);
  ++f.now;
  const Command command = f.next();
  f.ack(command);
  f.running();

  Fixture completion;
  completion.now = UINT32_MAX - 250;
  const Command outstanding = completion.next();
  completion.now += kCompletionTimeoutMs - 1;
  REQUIRE(completion.poll() == nullptr);
  REQUIRE(completion.machine.diagnostics().failures == 0);
  ++completion.now;
  REQUIRE(completion.poll(false) == nullptr);
  REQUIRE(completion.machine.diagnostics().failures == 1);
  completion.ack(outstanding);
  REQUIRE(completion.machine.diagnostics().failures == 1);

  Fixture backoff;
  backoff.now = UINT32_MAX - 500;
  const Command rejected = backoff.next();
  backoff.ack(rejected, 1);
  backoff.now += kRetryDelayMs - 1;
  REQUIRE(backoff.poll() == nullptr);
  ++backoff.now;
  REQUIRE(backoff.next().opcode == RESET);
}

void eventDecoder() {
  const std::vector<uint8_t> complete = {4, 0x0e, 4, 1, 0x08, 0x20, 0};
  uint16_t opcode = 0;
  uint8_t status = 255;
  REQUIRE(decodeCommandEvent(complete.data(), complete.size(), opcode, status));
  REQUIRE(opcode == ADVERTISING && status == 0);
  for (size_t length = 0; length < complete.size(); ++length)
    REQUIRE(!decodeCommandEvent(complete.data(), length, opcode, status));
  REQUIRE(!decodeCommandEvent(nullptr, 20, opcode, status));
  for (const unsigned field : {0U, 1U, 2U}) {
    auto malformed = complete;
    malformed[field] = field == 2 ? 5 : 0;
    REQUIRE(!decodeCommandEvent(malformed.data(), malformed.size(), opcode, status));
  }
  auto tooShortPayload = complete;
  tooShortPayload[2] = 3;
  REQUIRE(!decodeCommandEvent(tooShortPayload.data(), tooShortPayload.size(), opcode, status));
  std::vector<uint8_t> commandStatus = {4, 0x0f, 4, 0, 1, 0x03, 0x0c};
  REQUIRE(!decodeCommandEvent(commandStatus.data(), commandStatus.size(), opcode, status));
  commandStatus[3] = 0x0c;
  REQUIRE(decodeCommandEvent(commandStatus.data(), commandStatus.size(), opcode, status));
  REQUIRE(opcode == RESET && status == 0x0c);
  Fixture f;
  f.next();
  f.machine.complete(opcode, status, ++f.now);
  REQUIRE(f.machine.diagnostics().lastError == Error::CommandRejected);
  commandStatus[2] = 5;
  commandStatus.push_back(0);
  REQUIRE(!decodeCommandEvent(commandStatus.data(), commandStatus.size(), opcode, status));
}

void deviceServiceKeepsRunning() {
  Fixture unavailable;
  Fixture missing;
  missing.next();
  unsigned deviceHeartbeats = 0;
  for (unsigned tick = 0; tick < 20000; ++tick) {
    ++deviceHeartbeats; // Represents required sensor/timer/Wi-Fi service first.
    ++unavailable.now;
    ++missing.now;
    REQUIRE(unavailable.poll(true, false) == nullptr);
    REQUIRE(missing.poll() == nullptr);
  }
  REQUIRE(deviceHeartbeats == 20000);
  REQUIRE(unavailable.machine.diagnostics().sentCommands == 0);
  REQUIRE(unavailable.machine.diagnostics().failures > 1);
  REQUIRE(unavailable.machine.diagnostics().failures < 12); // Backoff, no retry burst.
  REQUIRE(missing.machine.diagnostics().sentCommands == 1);
  REQUIRE(missing.machine.diagnostics().failures == 1);
  // Fake time verifies progress, not real ESP32 scheduling or RF latency.
}

void startupFailureAndIdempotence() {
  StateMachine failed;
  failed.setDeviceName("valid");
  failed.start(false);
  failed.start(true);
  for (uint32_t now = 0; now < 10000; ++now) REQUIRE(failed.poll(now, true, true) == nullptr);
  REQUIRE(failed.diagnostics().lastError == Error::StartupFailed);
  REQUIRE(failed.diagnostics().failures == 1);
  Fixture f;
  f.running();
  f.machine.start(false);
  REQUIRE(f.machine.diagnostics().step == Step::Running);
  REQUIRE(f.poll() == nullptr);
}

int main(int argc, char** argv) {
  const std::map<std::string, std::function<void()>> tests = {
    {"normal_sequence", normalSequence}, {"copied_bounded_names", copiedAndBoundedNames},
    {"invalid_names_stop", invalidNamesStop}, {"latest_name_every_stage", latestNameAtEveryStage},
    {"clear_name_every_stage", clearNameAtEveryStage}, {"busy_gate", busyGate},
    {"send_ready_backoff", sendReadyBackoff}, {"each_rejected_command", eachRejectedCommand},
    {"missing_completion", missingCompletion}, {"wrong_opcode_delayed_consumption", wrongOpcodeAndDelayedConsumption},
    {"timely_completion_after_provisional_timeout", timelyCompletionAfterProvisionalTimeout},
    {"rollover", rollover}, {"event_decoder", eventDecoder},
    {"device_service_progress", deviceServiceKeepsRunning}, {"startup_failure_idempotence", startupFailureAndIdempotence}
  };
  try {
    if (argc == 2) {
      const auto test = tests.find(argv[1]);
      REQUIRE(test != tests.end());
      test->second();
      std::cout << "PASS: " << test->first << '\n';
    } else {
      for (const auto& test : tests) { test.second(); std::cout << "PASS: " << test.first << '\n'; }
    }
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
