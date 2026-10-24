"""Verify actual worker cancellation/result queue code cannot strand or replay a command."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

GLOVE = Path(__file__).resolve().parents[1]


def production_functions():
    source = (GLOVE / "wifi.cpp").read_text()
    functions = []
    for name in ("cancelQueuedCommand", "networkPollResult", "networkSubmit"):
        match = re.search(rf"(?:bool|void) {name}\([^)]*\) \{{", source)
        start, cursor, depth = match.start(), match.end(), 1
        while depth:
            char = source[cursor]
            depth += (char == "{") - (char == "}")
            cursor += 1
        functions.append(source[start:cursor])
    return "\n".join(functions)


HARNESS = r'''
#include "wifi_client.h"
#include <atomic>
#include <cassert>
#include <cstring>
using namespace iotglove;
constexpr int pdTRUE = 1;
struct Queue {
  unsigned char data[sizeof(GameEvent)]{};
  size_t size;
  bool full = false;
};
Queue commandQueue{{}, sizeof(GameEvent), false};
Queue resultQueue{{}, sizeof(GameResult), false};
Queue* commands = &commandQueue;
Queue* results = &resultQueue;
std::atomic<bool> commandBusy{false};
std::atomic<NetworkOtaStatus> otaState{NetworkOtaStatus::Idle};
int xQueueReceive(Queue* q, void* out, int) {
  if (!q->full) return 0;
  memcpy(out, q->data, q->size); q->full = false; return pdTRUE;
}
int xQueueSend(Queue* q, const void* in, int) {
  if (q->full) return 0;
  memcpy(q->data, in, q->size); q->full = true; return pdTRUE;
}
int xQueueOverwrite(Queue* q, const void* in) {
  memcpy(q->data, in, q->size); q->full = true; return pdTRUE;
}
unsigned traceCalls = 0;
GameEvent traced;
int tracedStage = -1, tracedStatus = -1;
namespace iotglove {
void blackBoxButtonCommand(const GameEvent& event, int stage, int status) {
  ++traceCalls; traced = event; tracedStage = stage; tracedStatus = status;
}
// PRODUCTION_FUNCTIONS
}
int main() {
  GameEvent event; event.sequence = 42; event.pressId = 0x80000001U; event.traceButton = true;
  assert(networkSubmit(event));
  cancelQueuedCommand();
  assert(!commandQueue.full && commandBusy.load());
  assert(traceCalls == 1 && traced.sequence == 42 && traced.pressId == 0x80000001U && traced.traceButton);
  assert(tracedStage == 6 && tracedStatus == int(GameResult::Status::Unknown));
  assert(!networkSubmit(event)); // No replay while the main loop owns token42.
  GameResult result;
  assert(networkPollResult(result));
  assert(result.sequence == 42 && result.status == GameResult::Status::Unknown);
  assert(!commandBusy.load());
  assert(!networkPollResult(result));
  cancelQueuedCommand(); // Empty cancellation cannot create phantom results.
  assert(traceCalls == 1);
  assert(!networkPollResult(result));
  event.sequence = 43;
  assert(networkSubmit(event));
  assert(!networkSubmit(event));
  GameEvent dequeued;
  assert(xQueueReceive(commands, &dequeued, 0));
  cancelQueuedCommand(); // An already-running mutation belongs to execute().
  assert(traceCalls == 1 && dequeued.pressId == 0x80000001U && dequeued.traceButton);
  assert(!networkPollResult(result));
  GameResult unresolved{dequeued.sequence, GameResult::Status::Unknown};
  xQueueOverwrite(results, &unresolved);
  assert(networkPollResult(result) && result.sequence == 43);
  for (auto state : {NetworkOtaStatus::Queued, NetworkOtaStatus::Running}) {
    otaState.store(state);
    assert(!networkSubmit(event) && !commandBusy.load());
  }
  otaState.store(NetworkOtaStatus::Idle);
  assert(networkSubmit(event));
}
'''


class NetworkCancellationTest(unittest.TestCase):
    def test_production_queue_cancellation(self):
        with tempfile.TemporaryDirectory(prefix="iotglove-command-cancel-") as work:
            source = Path(work) / "cancel.cpp"
            binary = Path(work) / "cancel"
            source.write_text(HARNESS.replace("// PRODUCTION_FUNCTIONS", production_functions()))
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                            "-Werror", "-pedantic", "-include", "initializer_list", "-I", str(GLOVE),
                            str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
