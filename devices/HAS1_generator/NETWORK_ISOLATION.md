# Generator networking with the existing server

## Compatibility and release status

Completion and contribution transmission are enabled in the normal build using
existing `Send device_state=battery_max/repaired` and `Situation generator_gauge`
requests. No new server endpoint, generation parameter or capability is required.
The earlier development-only transmission block and deliberate production-build
error have been removed at the user's request.

Completion allows one transport attempt per local operation, followed by
ReceiveMine state checks. Enqueue failure or definite cancellation before
dispatch can be retried after fresh gameplay checks. After dispatch, a timeout,
HTTP failure or missing confirmation does not cause an automatic repeat of that
Send, with one narrow exception: when the TCP connect itself failed or the
shared deadline expired while still connecting, no request byte was written and
the server cannot have applied the mutation. That undelivered Send may be
resent, at most two more times (three attempts per transaction), under the same
token and with the original wire expectations. The resend is armed only by a
successful ReceiveMine verify, taken after the failed attempt, that still shows
the state the transaction started from; the cached state is never trusted for
this. A queued resend is withdrawn (without resetting the transaction) while
WiFi is offline or after five seconds in the queue, so it cannot sit through an
outage and land after staff changed the state. The snapshot observer drops the
pending resend as soon as any other authoritative device state is seen, and the
dispatch guard still rejects a resend whose wires or game generation changed.
A failure after any request byte (write, header or body stage) stays uncertain
and is never repeated. HTTP 200 by itself is not completion. Reads remain
retryable and do not block gameplay input or the network polling loop.

Contributions retain a bounded buffer only until queue acceptance; uncertain
transmission is not retried automatically. This avoids introducing blind
repetition into APIs that can apply duplicates. It is not durable exactly-once
delivery across reboot, and it can lose an event when a request never arrives.

Server changes are not a prerequisite for this compatibility version. Field
validation is still needed before claiming that the resets are resolved. No
firmware release or installation has been performed by this change.

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
  confirmation rather than assuming a queued Send succeeded.
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
- GitHub OTA needs memory the running device does not have spare. The core's
  mbedTLS uses fixed 16 KiB in/out record buffers, about 45 KiB per HTTPS request
  including two ~17 KiB contiguous blocks, while a generator runs with ~48 KiB
  free (largest block 31-45 KiB). Every GitHub request failed at connect (HTTP -1).
  Before `ota.check()` the main task therefore waits (up to 15 seconds after the
  request) for the blackbox MQTT session to end, then stops the BLE controller
  (`Has1BleBeacon::suspend()`), logs `[NET] OTA memory` heap/largest before and
  after, and runs the check. A successful update restarts the device. A failed or
  skipped one restarts BLE (`resume()`), which replays its full advertising setup,
  and logs `[NET] OTA returned without update`. If BLE does not come back, the
  device restarts so no game runs without its beacon. GitHub maintenance starts
  only when `device_state` changes to `github` (including the first snapshot after
  boot), so a failed attempt is retried after the state changes away and back or
  after a reboot, as before. A restart for BLE therefore repeats the attempt while
  the server stays at `github`, until staff change the state. The local HTTP update
  (`device_state=update`) needs no TLS and keeps its path without these steps, so
  it remains the fallback if the new path misbehaves.

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

The existing server does not check a game generation for completion or
contributions; duplicate `repaired` can decrement its counter again. A request
already sent can still arrive during the next game despite local cancellation.
The existing APIs cannot resolve that case fully on the client. Polling can also
miss a complete reset/start transition while the device is offline.

If a completion request never applied, the device keeps checking the state but
does not repeat the mutation automatically once any request byte has been
written; only a connect-stage failure is resent within the bounded budget above.
Unconfirmed completion is logged at a bounded rate after 30 seconds together
with the number of Send attempts used. Staff may need to confirm/correct the
state in the existing OS. A future server generation/idempotency contract can
remove this tradeoff; merely appending an unsupported `gen` parameter cannot.
An ignored/error response, including HTTP 200 with an application-level rejection,
must not be interpreted as proof of completion.

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
With the existing server, test successful completion, lost replies after a
committed mutation, requests that never applied, and staff recovery. Observe
cross-game delayed requests as a remaining limitation; this firmware cannot
guarantee that the server rejects them.

## Verification (2026-10-22)

Independent review of the network isolation resolved the scan-buffer race, offline maintenance drain,
stale contribution session, epoch restoration of completed states, tag queue
exhaustion and completion verification after card removal. Compatibility review
also fixed operation-result mixups, completion suppression by a prior echo flag,
queued writes overriding newer server states, and cancellation racing dispatch.
The host suite verifies normal completion, lost ACK, unchanged/error/ignored
responses, same-stage re-entry, definite pre-dispatch retry, staff override and
tagged-stage restoration without introducing automatic mutation retries after a
request byte was written. It also covers the connect-stage resend (added
2026-10-23 after a field case where a completion Send was lost to a 2 s connect
timeout and staff had to force the state): armed only by a fresh start-state
verify, bounded to three attempts, same token, deferred to the next verify while
the queue is full, withdrawn while offline or after five seconds in the queue,
dropped after a staff override or any other authoritative state change,
rejected at dispatch when wires changed, never issued for a previous game
generation, and not mistaken for the device's own echo. With these cases the
network suite is 48 groups (was 37). Remaining known limits: the five-second
queue lifetime is enforced from the main loop, so a main-loop stall (audio
wait, LED blink) extends it by that stall; and when the server leaves the start
state while a resend is merely pending the transaction stays unresolved, while
an already queued resend takes the existing queued-cancel reset.

| Check | Result |
| --- | --- |
| Bounded transport, including real loopback sockets | 48 cases passed, ASan/UBSan |
| Existing-server worker/completion integration | 37 groups passed, ASan/UBSan; includes actual `DataChanged`, `ActivateFunc`, `BatteryFinish` and `StartFinish` |
| Wi-Fi | 13 controller cases plus scan barrier, missing-event fault, callback concurrency and blocking-path guards passed |
| Wire / polling and crash reporting | 36 / 4 groups passed |
| Diagnostic / Telnet / Telnet lifecycle | 10 / 14 / 15 groups passed |
| DFPlayer / NeoPixel / legacy HTTP / tagger | 20 / 8 / 9 groups and tagger suite passed |
| TTGO T1, Arduino-ESP32 3.3.11 | Compile passed: 1,339,533 bytes flash (68%), 90,532 bytes globals (27%) |

The compile used a placeholder signing secret and is not an installable release.
Build memory figures do not measure runtime heap or worker stack headroom.
No firmware was flashed or published. Same-game contribution closure uses the
last sampled gauge count; pulses not yet read from PCNT at the transition are
not guaranteed to be included.
