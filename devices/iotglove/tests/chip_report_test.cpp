#include "chip_report.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
using namespace iotglove;

static void initialAndDedup(bool present) {
  ChipReportPolicy policy;
  ChipReportPolicy::Request request;
  assert(!policy.begin(0, request));
  policy.observe(present);
  assert(!policy.begin(0, request));
  policy.bind("G1P1", false, -1);
  assert(policy.begin(0, request));
  assert(request.present == present && !strcmp(request.device, "G1P1"));
  assert(!policy.begin(1, request));
  policy.finish(request, true, 1);
  for (unsigned i = 2; i < 20; ++i) {
    policy.observe(present);
    policy.bind("G1P1", true, present ? 1 : 0);
    assert(!policy.due(i));
  }
  policy.observe(!present);
  assert(policy.begin(20, request));
  assert(request.present != present);
  policy.finish(request, true, 21);
  assert(!policy.due(22));
}

static void newestOnlyAndConcurrentReadback() {
  ChipReportPolicy policy;
  ChipReportPolicy::Request old, newest;
  policy.bind("G2P8", false, -1);
  policy.observe(false);
  assert(policy.begin(10, old));
  policy.observe(true);  // HTTP for old=false is still in progress.
  policy.finish(old, true, 11);
  assert(policy.due(11));
  assert(policy.begin(11, newest) && newest.present);
  policy.finish(old, true, 12);  // Stale completion cannot release the new request.
  assert(!policy.due(12));
  policy.finish(newest, true, 13);
  assert(!policy.due(13));
  policy.observe(false); policy.observe(true);  // Unsampled edges are not replayed.
  assert(!policy.due(14));
  policy.disconnect();
  policy.observe(false); policy.observe(true); policy.observe(false);
  assert(!policy.due(15));
  policy.bind("G2P8", true, 1);
  assert(policy.begin(16, newest) && !newest.present);
}

static void failedWriteAndRetry() {
  ChipReportPolicy policy;
  ChipReportPolicy::Request request;
  policy.observe(false); policy.bind("G1P2", false, -1);
  assert(policy.begin(0, request)); policy.finish(request, true, 1);
  policy.observe(true);
  assert(policy.begin(2, request));
  policy.observe(false);
  // The unconfirmed true write might have reached the server, so old false ACK
  // cannot suppress restoring the latest false state.
  policy.finish(request, false, 3);
  assert(!policy.due(5002));
  policy.bind("G1P2", false, -1);  // Server-ready=0 cannot bypass backoff.
  assert(!policy.due(5002));
  assert(policy.begin(5003, request) && !request.present);
  policy.finish(request, true, 5004);
  assert(!policy.due(5005));
}

static void identityAndServerRestart() {
  ChipReportPolicy policy;
  ChipReportPolicy::Request original, changed;
  policy.observe(true); policy.bind("G1P1", false, -1);
  assert(policy.begin(0, original)); policy.finish(original, true, 1);
  policy.bind("G1P1", false, -1);  // Server-only restart drops registration.
  assert(policy.begin(2, original)); policy.finish(original, true, 3);
  assert(!policy.due(3));
  policy.bind("G1P2", true, 1);  // Reassignment needs its own current-state sync.
  assert(policy.begin(4, original) && !strcmp(original.device, "G1P2"));
  policy.bind("G1P3", true, 1);
  policy.finish(original, true, 5);  // Old target's readback is not the new ACK.
  assert(!policy.due(5004));
  assert(policy.begin(5005, changed) && !strcmp(changed.device, "G1P3"));
  policy.finish(changed, true, 5006);
  policy.disconnect(); policy.bind("G1P3", true, 1);
  assert(policy.begin(5007, changed));
}

static void serverValueChanges() {
  ChipReportPolicy policy;
  ChipReportPolicy::Request request;
  policy.observe(false); policy.bind("G1P1", false, 1);
  assert(policy.begin(0, request)); policy.finish(request, true, 1);
  policy.bind("G1P1", true, 0); assert(!policy.due(2));
  policy.bind("G1P1", true, 1); assert(policy.begin(3, request) && !request.present);
  policy.finish(request, true, 4);
  policy.bind("G1P1", true, -1); assert(policy.begin(5, request));
  policy.finish(request, false, 6);
  policy.bind("G1P1", true, 2); assert(!policy.due(5005));
  assert(policy.begin(5006, request) && !request.present);
  policy.observe(true);
  policy.bind("G1P1", true, 0); policy.finish(request, true, 5007);
  assert(policy.due(5007));  // Fresh old-value readback still leaves new desired pending.
}

static void identityValidationAndWrap() {
  const char* rejected[] = {nullptr, "", "G", "G1", "G1P", "G1P-1", "G1P1234", "G3P1", "G9P1", "G1P1x", "g1p1"};
  for (const char* name : rejected) {
    ChipReportPolicy policy;
    policy.observe(true); policy.bind(name, true, 1);
    assert(!policy.due(0));
  }
  assert(ChipReportPolicy::liveDevice("G1P1"));
  assert(ChipReportPolicy::liveDevice("G2P99"));
  ChipReportPolicy policy;
  ChipReportPolicy::Request request;
  policy.observe(true); policy.bind("G1P8", false, -1);
  const uint32_t failure = UINT32_MAX - 1000;
  assert(policy.begin(failure - 1, request)); policy.finish(request, false, failure);
  assert(!policy.due(failure + 4999u));
  assert(policy.begin(failure + 5000u, request));
  policy.finish(request, true, failure + 5001u);
  assert(!policy.due(failure + 10000u));
}

int main() {
  initialAndDedup(false); initialAndDedup(true);
  newestOnlyAndConcurrentReadback(); failedWriteAndRetry();
  identityAndServerRestart(); serverValueChanges(); identityValidationAndWrap();
  puts("chip report policy tests passed");
}
