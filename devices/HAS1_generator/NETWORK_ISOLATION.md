# Generator network isolation — development build

## Release status

**Do not install this development build in the store.** The inspected server has
no game-generation check for `Send device_state=battery_max/repaired`, and a
duplicate `repaired` can decrement `left_generator` again. Contributions also use
the session at arrival rather than the originating game.

Completion and contribution transmission are disabled by default until that
server contract is implemented and verified. Inputs, wire CAS, polling, local
diagnostics and server-directed state transitions can be tested. A normal local
game cannot finish with this gate enabled. `GENERATOR_ALLOW_UNFENCED_COMPLETION=1`
exists only for controlled legacy-protocol tests; it is not a production fix.

The deployment workflow defines `GENERATOR_PRODUCTION_BUILD`, and
`network_release_gate.h` deliberately rejects that build, including when the
legacy bench flag is enabled. Removing the release gate requires server contract
implementation, integration evidence and hardware validation. Firmware version
was not incremented, and no firmware release was published.

## What changed

- A single worker owns local HTTP sockets and Wi-Fi connection recovery. Main
  queues requests without waiting, then validates and applies results. Main alone
  modifies `my`, `tag`, `shift_machine` and calls `DataChanged`.
- There are eight fixed request slots and one response mailbox. Optional reports
  leave space for polling/state repair. No per-request task creation or synchronous
  fallback occurs when capacity is exhausted. Unsent contribution retention is
  bounded separately; uncertainty after transmission is never blindly retried.
- Plain numeric-IP GET requests share a 2000ms deadline across connect, write,
  headers and body. This is a configurable bench starting value, not a proven
  best timeout for the current PHP proxy. Header and body sizes are bounded.
  See the transport library's `tests/BOUNDED_TRANSPORT.md` for framing and limits.
- The deadline starts at dispatch, not enqueue. Queue wait is logged separately
  from transport duration; response-consumption wait is not measured separately.
  Offline queued work does not stop the game
  loop. OS socket cleanup may return after its deadline; the result reports that
  overrun, and the worker watchdog remains the final guard against a stuck call.
- Normal polling waits two seconds after completion. Missed ticks are skipped.
  Failures use 2/4/8-second backoff plus up to 200ms jitter. Server heartbeat/offline
  thresholds must be checked before selecting production values.
- Wire sampling retains the one-second startup and symmetric 8/10 qualification.
  CAS epoch/revision validation remains in place. Queued wire values coalesce;
  changed physical/game generations invalidate old work/results.
- Tag lookup is asynchronous and retryable while a card remains present. Old
  card results cannot approve a replacement card. Completion waits for server
  confirmation in the bench path rather than assuming a queued Send succeeded.
- External Google Sheet uploads are removed from boot/game execution. RTC/Telnet
  diagnostics and queued local crash reports remain. A full queue leaves the
  initial report pending; transport failure is visible without deleting retained
  local evidence.
- Boot no longer erases the entire NVS partition. Initial server state is obtained
  asynchronously before normal game transitions are applied.
- Wi-Fi recovery uses an opt-in asynchronous controller. Legacy `HAS2_Wifi`
  callers retain their existing behavior. Ordinary AP/server unavailability is
  handled as an offline/retry condition, not an immediate reboot instruction.
  Scan buffers are retained until the driver's completion event establishes safe
  ownership. A scan that exceeds six seconds is cancelled; if its completion
  event is still missing four seconds later, a driver fault is latched. Main
  records the fault and the worker stops feeding its own watchdog. Thus a wedged
  driver can still cause a reset, separately from ordinary connection failures.
- OTA is an explicit maintenance path. It is not included in the promise of
  uninterrupted normal gameplay. Worker and main state ownership still applies.
  Cancelled queued work can drain offline; maintenance aborts after ten seconds
  without connectivity rather than holding the normal queue indefinitely.

Network diagnostics identify request kind/id/generation, queue delay, transport
phase and elapsed time. The worker publishes a small progress snapshot; main
records it without requiring the worker to format or transmit Telnet output.
Wi-Fi disconnect events likewise publish a protected reason/time/sequence snapshot
for main to consume, without logging from the event callback.
Retained diagnostic evidence is bounded and cannot guarantee preservation of
every event across power loss or a long outage.

## Correctness boundaries

Local request generations reject queued work and stale responses. They cannot
undo a mutation already sent to the server. The current wire CAS protects its
own writes; it does not prove that completion or contribution APIs are protected.

The eventual server contract must verify the originating game generation and
handle duplicate completion/contribution IDs atomically with its state changes.
It must reject an old game's operation even after a new game becomes active.
HTTP 200 alone is not completion: `success:true, ignored:STALE_GENERATION` means
the operation was not applied. An explicit capability/version and real endpoint
tests are required before the production gate is removed. Merely appending `gen`
to a legacy URL is insufficient because old code can ignore unknown parameters.

Existing input, audio and LED code can have their own bounded delays. Moving
network waits off main does not establish a universal maximum main-loop duration.
During an AP outage live Telnet is unavailable; local bounded logs are retained
and replayed after connection recovery.

## Verification commands

Use ArduinoJson 7.4.3 headers and the pinned Arduino-ESP32 3.3.11 toolchain.

```sh
python3 devices/HAS1_generator/tests/run_poll_schedule_tests.py
python3 libraries/HAS2_Wifi/tests/run_connection_tests.py
SANITIZE=1 python3 libraries/HAS2_Wifi/tests/run_bounded_transport_tests.py
ARDUINOJSON_INCLUDE=/path/to/ArduinoJson/src python3 devices/HAS1_generator/tests/run_network_tests.py
ARDUINOJSON_INCLUDE=/path/to/ArduinoJson/src python3 devices/HAS1_generator/tests/run_wire_tests.py
ARDUINOJSON_INCLUDE=/path/to/ArduinoJson/src python3 devices/HAS1_generator/tests/run_http_body_tests.py
```

Also run the existing tagger, diagnostic, Telnet, DFPlayer and NeoPixel suites.
The network harness executes production worker/facade code with controlled
scheduling. It is not proof of actual FreeRTOS race freedom or RF behavior.
Transport tests include a real loopback socket but do not reproduce the store AP.

Required hardware checks before release: delayed/partial/no server responses,
server and AP outage/recovery, wire changes while requests are pending, tag swap,
new-game transition during an in-flight mutation, explicit employee reboot,
OTA maintenance, repeated startup and worker stack/heap/socket high-water marks.
Run the obsolete-completion and duplicate-`repaired` cases against the updated
server, including lost replies after a committed mutation.

## Recorded verification (2026-10-22)

Independent review resolved the scan-buffer race, offline maintenance drain,
stale contribution session, epoch restoration of completed states, tag queue
exhaustion and completion verification after card removal.

| Check | Result |
| --- | --- |
| Bounded transport, including real loopback sockets | 48 cases passed, ASan/UBSan |
| Network worker/facade, wire and contribution integration | Default 20 / legacy bench 21 groups passed, ASan/UBSan; includes actual `DataChanged` |
| Wi-Fi | 13 controller cases plus scan barrier, missing-event fault, callback concurrency and blocking-path guards passed |
| Wire / polling and release gate | 36 / 5 groups passed |
| Diagnostic / Telnet / Telnet lifecycle | 10 / 14 / 15 groups passed |
| DFPlayer / NeoPixel / legacy HTTP / tagger | 20 / 8 / 9 groups and tagger suite passed |
| TTGO T1, Arduino-ESP32 3.3.11 | Compile passed: 1,335,633 bytes flash (67%), 90,388 bytes globals (27%) |

The compile used a placeholder signing secret and is not an installable release.
Build memory figures do not measure runtime heap or worker stack headroom.
No firmware was flashed or published. Same-game contribution closure uses the
last sampled gauge count; pulses not yet read from PCNT at the transition are
not guaranteed to be included.
