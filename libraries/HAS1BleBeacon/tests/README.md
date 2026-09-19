# BLE state machine host tests

Run from the repository root:

```sh
python3 libraries/HAS1BleBeacon/tests/run_tests.py
```

Requires Python 3 and `clang++`. The runner compiles the real `src/BeaconState.cpp`
with strict warnings, AddressSanitizer and UndefinedBehaviorSanitizer in a temporary
directory. It does not require Arduino libraries or connected hardware.

The 15 scenario groups exercise the HCI command contract and advertisement bytes,
one outstanding command, completion ownership, copied/bounded names, name changes
and clearing during every setup stage, busy gating, command rejection, late/missing
completion, send readiness and retry deadlines, clock rollover, malformed H4 events,
and continued simulated device service while BLE is unavailable.

Seven additional scenarios compile the real `src/HAS1BleBeacon.cpp` adapter against
small Arduino/controller stubs. Each runs in a fresh process to exercise boot-lifetime
state: controller/callback startup failure, idempotent BLE-only startup, synchronous
controller replies during send, successful Command Status versus final completion,
timely mailbox replies consumed after a busy loop, wrong/duplicate replies, and
continued simulated service when VHCI never becomes available.
The critical-section stub checks balanced, non-nested lock use and rejects clock or
controller operations while the mailbox lock is held; it does not simulate
ESP32 cross-core scheduling.

Simulated time and controller replies verify state-machine behavior. These tests do
not measure ESP32 loop latency, controller heap, Wi-Fi/BLE coexistence, RF reception,
or physical sensor/motor response.
