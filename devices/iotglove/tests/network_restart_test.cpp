#include "network_restart.h"
#include <cassert>
#include <cstdint>

using namespace iotglove;

int main() {
  NetworkRestartHistory history{};
  NetworkRestartPolicy::boot(history, false, false);
  assert(NetworkRestartPolicy::allowed(history));
  assert(NetworkRestartPolicy::request(history));
  NetworkRestartPolicy::boot(history, true, false);
  assert(history.attempts == 1 && !history.requested);
  assert(NetworkRestartPolicy::request(history));
  NetworkRestartPolicy::boot(history, true, false);
  assert(!NetworkRestartPolicy::allowed(history));
  assert(!NetworkRestartPolicy::request(history));
  NetworkRestartPolicy::boot(history, false, false);  // Power cycle.
  assert(history.attempts == 0);
  NetworkRestartPolicy::boot(history, false, true);   // Main watchdog panic.
  assert(history.attempts == 1);
  NetworkRestartPolicy::boot(history, false, true);
  assert(!NetworkRestartPolicy::allowed(history));

  NetworkRestartPolicy frozen;
  for (uint32_t now = 0; now < 1800000U; now += 1000U)
    assert(!frozen.observe(now, 1, false));
  NetworkRestartPolicy live;
  assert(!live.observe(0, 1, false));
  for (uint32_t now = 1000; now < 600000; now += 1000)
    assert(!live.observe(now, now / 1000, false));
  assert(live.observe(600000, 600, false)); // Includes server-down completed attempts.
  assert(!live.observe(600001, 601, true));
  assert(!live.observe(1200000, 602, false)); // Stall discards healthy interval.

  NetworkRestartPolicy rollover;
  const uint32_t start = UINT32_MAX - 100000;
  assert(!rollover.observe(start, 0, false));
  for (uint32_t elapsed = 1000; elapsed < 600000; elapsed += 1000)
    assert(!rollover.observe(start + elapsed, elapsed, false));
  assert(rollover.observe(start + 600000U, 600000, false));
}
