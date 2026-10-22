"""Exercise the production OTA dispatch and watchdog hooks with legacy clients."""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
GLOVE = ROOT / "devices/iotglove"

HARNESS = r'''
#include "wifi_client.h"
#include <IoTGloveOtaClient.h>
#include <atomic>
#include <cassert>
#include <cstring>
#include <string>
using namespace iotglove;
using String = std::string;
uint32_t now = 0;
uint32_t millis() { return now; }
int diagnosticsMux;
#define portENTER_CRITICAL(p) (void)(p)
#define portEXIT_CRITICAL(p) (void)(p)
NetworkDiagnostics diagnostics;
bool otaOperation = false;
std::atomic<NetworkOtaStatus> otaState{NetworkOtaStatus::Idle};
struct OtaRequest { uint32_t targetVersion = 0; char sourceCommand[40] = {}; };
int version = 6000, partitionVersion = 2;
const char HMAC_SECRET[] = "test-only-key";
ServerSnapshot latest;
char lastDeviceState[64] = "update";
bool refreshWorks = true;
int refreshCalls = 0, acknowledged = 0, latestCalls = 0, pinnedCalls = 0;
enum class Outcome { Skipped, Failed, Reboot };
Outcome outcome = Outcome::Skipped;
struct Restart {};
enum class HAS2HttpPhase { Connect, Body, Complete };
struct HAS2HttpResult {
  uint8_t error = 0; HAS2HttpPhase phase = HAS2HttpPhase::Complete;
  int status = 200; uint32_t elapsedMs = 0, bytes = 0;
};
struct Wifi { HAS2HttpResult LastHttpResult() { return {}; } } wifi;
void observeResponseBackoff(const HAS2HttpResult&) {}
void remoteConsoleLogf(const char*, ...) {}
void httpProgress(HAS2HttpPhase, uint32_t, void*);
void assertOtaSafe() {
  assert(otaOperation && diagnostics.phase == NetworkPhase::Ota);
  now += 180000;
  assert(!networkWorkerStalled(diagnostics, now));
  assert(otaState.load() == NetworkOtaStatus::Running);
}
bool refresh() {
  ++refreshCalls;
  const bool inOta = otaOperation;
  httpProgress(HAS2HttpPhase::Connect, 0, nullptr);
  httpProgress(HAS2HttpPhase::Body, 100, nullptr);
  httpProgress(HAS2HttpPhase::Complete, 100, nullptr);
  if (inOta) assertOtaSafe();
  return refreshWorks;
}
bool send(const char* field, const String& value) {
  assertOtaSafe();
  assert(std::string(field) == "device_state" && value == "setting");
  httpProgress(HAS2HttpPhase::Connect, 0, nullptr);
  httpProgress(HAS2HttpPhase::Complete, 0, nullptr);
  assert(diagnostics.phase == NetworkPhase::Ota);
  ++acknowledged;
  return true;
}
struct SecureOTA {
  std::function<void()> skipped, success;
  SecureOTA(const char* image, const char* versionUrl, const char* signature,
            const char* secret, int current) {
    const std::string base = "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/iotglove/";
    assert(image == base + "update.bin");
    assert(versionUrl == base + "version.txt");
    assert(signature == base + "update.sig");
    assert(std::string(secret) == HMAC_SECRET && current == version);
  }
  void setOnSkip(std::function<void()> callback) { skipped = callback; }
  void setOnSuccess(std::function<void()> callback) { success = callback; }
  void check() {
    ++latestCalls;
    assertOtaSafe();
    if (outcome == Outcome::Skipped) skipped();
    if (outcome == Outcome::Reboot) { success(); throw Restart{}; }
  }
};
namespace iotglove { namespace ota {
CheckResult checkPinned(const char* board, uint32_t target, uint32_t current,
                        uint32_t partition, const char* secret,
                        std::function<void()> onSuccess) {
  ++pinnedCalls;
  assert(std::string(board) == "iotglove" && target == 6010);
  assert(current == 6000 && partition == 2 && std::string(secret) == HMAC_SECRET);
  assertOtaSafe();
  if (outcome == Outcome::Reboot) { onSuccess(); throw Restart{}; }
  return outcome == Outcome::Skipped ? CheckResult::Skipped : CheckResult::Failed;
}
// Deliberately no fetchLatestTarget definition: any new use must fail to link.
} }
'''

CASES = r'''
void reset() {
  now = 0; diagnostics = NetworkDiagnostics{}; otaOperation = false;
  otaState.store(NetworkOtaStatus::Queued); latest = ServerSnapshot{};
  refreshWorks = true; refreshCalls = acknowledged = latestCalls = pinnedCalls = 0;
  std::strcpy(lastDeviceState, "update");
  outcome = Outcome::Skipped;
}
void checkReturned(NetworkOtaStatus status) {
  assert(otaState.load() == status && !otaOperation);
  assert(diagnostics.phase == NetworkPhase::Idle);
  assert(!networkWorkerStalled(diagnostics, now + 19999));
  assert(networkWorkerStalled(diagnostics, now + 20000));
  // Actual next-request hook must re-enable ordinary HTTP stall detection.
  httpProgress(HAS2HttpPhase::Connect, 0, nullptr);
  assert(diagnostics.phase == NetworkPhase::Http);
  assert(networkWorkerStalled(diagnostics, now + 20000));
}
int main() {
  OtaRequest request;
  std::strcpy(request.sourceCommand, "update");
#ifdef IOTGLOVE_COMPILE_ONLY
  performOta(request);
  assert(otaState.load() == NetworkOtaStatus::Failed);
  assert(latestCalls == 0 && pinnedCalls == 0 && refreshCalls == 0);
#else
  for (uint32_t target : {0U, 6010U}) {
    for (auto result : {Outcome::Skipped, Outcome::Failed, Outcome::Reboot}) {
      reset(); request.targetVersion = target; outcome = result;
      bool restarted = false;
      try { performOta(request); } catch (const Restart&) { restarted = true; }
      assert(latestCalls == (target == 0 ? 1 : 0));
      assert(pinnedCalls == (target == 0 ? 0 : 1));
      assert(acknowledged == (result == Outcome::Failed ? 0 : 1));
      assert(restarted == (result == Outcome::Reboot));
      if (!restarted)
        checkReturned(result == Outcome::Skipped ? NetworkOtaStatus::Skipped : NetworkOtaStatus::Failed);
    }
    reset(); request.targetVersion = target;
    std::strcpy(lastDeviceState, "update_9999");
    performOta(request);
    assert(acknowledged == 0); // A newer server command is never cleared.
    checkReturned(NetworkOtaStatus::Skipped);
    reset(); request.sourceCommand[0] = 0;
    performOta(request);
    assert(acknowledged == 0 && refreshCalls == 1); // USB owns no server command.
    checkReturned(NetworkOtaStatus::Skipped);
    std::strcpy(request.sourceCommand, "update");
  }
  for (auto phase : {Phase::Active, Phase::Photo}) {
    reset(); latest.phase = phase;
    performOta(request);
    assert(latestCalls == 0 && pinnedCalls == 0 && !otaOperation);
    assert(otaState.load() == NetworkOtaStatus::Failed);
  }
  reset(); refreshWorks = false;
  performOta(request);
  assert(latestCalls == 0 && pinnedCalls == 0 && !otaOperation);
  assert(otaState.load() == NetworkOtaStatus::Failed);
#endif
}
'''


class OtaCompatibilityTests(unittest.TestCase):
    def test_production_legacy_dispatch_and_monitoring(self):
        source = (GLOVE / "wifi.cpp").read_text()
        release = re.search(r"constexpr char kRelease\[\] = .*?;", source)
        self.assertIsNotNone(release)
        functions = []
        for name in ["beginOperation", "finishOperation", "httpProgress",
                     "completeOtaCommand", "performOta"]:
            match = re.search(r"void " + name + r"\(.*?\n\}\n", source, re.S)
            self.assertIsNotNone(match, name)
            functions.append(match.group())
        with tempfile.TemporaryDirectory(prefix="iotglove-ota-compat-") as work:
            cpp = Path(work) / "test.cpp"
            cpp.write_text(HARNESS + release.group() + "\n" + "\n".join(functions) + CASES)
            for compile_only in [False, True]:
                with self.subTest(compile_only=compile_only):
                    binary = Path(work) / ("compile-only" if compile_only else "runtime")
                    command = [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                               "-Werror", "-pedantic", "-I", str(GLOVE), "-I",
                               str(ROOT / "libraries/IoTGloveProtocol/src")]
                    if compile_only:
                        command.append("-DIOTGLOVE_COMPILE_ONLY=1")
                    subprocess.run(command + [str(cpp), "-o", str(binary)], check=True)
                    subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
