# Generator starter/tagger regression checks

Run from the repository root:

```sh
python3 devices/HAS1_generator/tests/run_tagger_tests.py
```

The host harness compiles the production gauge renderer, gauge animation, tagger
lifecycle/feedback, state-change dispatcher, and decay callback with fake pixels, clock, audio, and encoder.
It checks retained blue pixels, purple background, pending charge animation,
existing decay cadence, audio-before-blink ordering, decay during feedback,
restoration, interrupted feedback, other stages, and an empty gauge. Dispatch
checks include simultaneous battery changes, a pending local-send guard, and
legacy starter_finish restoration.

Hardware integration still needs a generator: RFID roles/removal/re-entry, PCNT
input suppression, server-driven transitions (including simultaneous battery
changes), MP3 completion/timeout, and a new round while feedback is active.
The harness does not emulate HTTP, PN532, DFPlayer UART, or the Arduino scheduler.

## Telnet logging regression checks

```sh
python3 devices/HAS1_generator/tests/run_telnet_tests.py
python3 devices/HAS1_generator/tests/run_telnet_lifecycle_tests.py
```

The host harness exercises the production RAM history and bounded sender with
AddressSanitizer/UndefinedBehaviorSanitizer: partial sends, replay/live ordering,
backpressure, overwritten history, per-loop send limits, disconnects, ESP-IDF
message truncation, and preservation of the original reset report. It does not
simulate FreeRTOS concurrency or a physical Wi-Fi link. See
[Telnet logging](../TELNET_LOGGING.md) for capture scope and limits. The generator
harnesses run in the `Generator tests` pull-request workflow.

The lifecycle harness also compiles the actual session start, drain, expiry and
main Telnet loop with controlled sockets and a clock. It checks 200 reconnects,
stalled-reader recovery after 5 seconds, acceptance of a queued replacement in
the same loop, input-flood budgets, zero-byte/EINTR sends, partial-send progress,
true idle, long main-loop gaps after progress, millis wrap, slow replay larger
than the control buffer, fatal sends and invalid descriptors. Incoming data never
extends a stalled-output deadline. The tests model socket API return values;
they do not establish the wall-clock latency of all real socket operations.

## Absolute wire synchronization regression checks

Use ArduinoJson **v7.4.3** headers, matching the firmware build:

```sh
ARDUINOJSON_INCLUDE=/path/to/ArduinoJson/src python3 devices/HAS1_generator/tests/run_wire_tests.py
```

The ASan/UBSan harness compiles production `wire.ino`, the state header, and
`wire_http.ino` against fake GPIO/server/HTTP streams, a deterministic task clock,
and the actual ArduinoJson parser. It drives the production static sampler task
with fake FreeRTOS primitives, including samples while synchronous HTTP advances
the clock. Checks cover the 100 ms sampling interval, a real ten-sample per-pin window,
and the initial one-second wait. Exact 8/10 and 9/10 votes qualify, while 7/10
remains ambiguous; disconnection uses the symmetric rule. Alternating equal-count
masks must remain ambiguous, while staggered per-pin noise can qualify all pins.
Sliding rise/fall checks retain the last display count but block publication and
completion during ambiguous windows. Completion reads no raw GPIO and tolerates a
last sample that disagrees with a valid vote. Long gaps cannot replace ten real
samples; already-qualified state has no age expiry if sampling stops. Failed task
creation, millis rollover, and resets clearing the window are also covered.
Critical-section checks keep fake logging, rendering, audio, and HTTP outside the
state lock.

Other cases cover sub-threshold transients, 0–4 counts, LG's 3-pack threshold, lost ACKs,
latest-value retries, the 2-second failure backoff, and successful updates without
a blanket 2-second gate. Transport cleanup is checked with a continuously
replenished unread response tail: success, conflict, status errors, invalid lengths,
incomplete bodies, and invalid JSON all stop the socket before HTTP cleanup.
A disconnect between headers and body is rejected without dereferencing a null stream.
These checks model the core's drain behavior; they do not bound header reception.
CAS fencing, new rounds/identity/epoch, preserved repaired
state, strict response validation, and response size/deadline limits remain covered.
The actual completion functions are also compiled with fake audio and HTTP to
check that battery completion yields while audio is pending, rechecks readiness
after audio and the state send, and does not repeat its announcement or send.
A server-declared `battery_max`/`starter_finish` (dashboard override) for a cycle
this device did not complete itself bypasses the physical wire gate until the next
charge cycle; it still reports physical counts and does not re-send `battery_max`.
The real `ActivateFunc` is compiled to check that a self-completed cycle keeps the gate.

These deterministic checks do not establish real FreeRTOS concurrency, task
latency, a real network, DFPlayer UART timing, or electrical noise behavior. Field
hardware validation has not been performed for this change, and the harness does
not guarantee a fixed connection-to-completion time. See
[the wire protocol and deployment sequence](../WIRE_SYNC.md). The Generator tests
workflow fetches the pinned JSON headers and runs this harness.

## Audio and HTTP stall regressions

```sh
python3 devices/HAS1_generator/tests/run_dfplayer_tests.py
ARDUINOJSON_INCLUDE=/path/to/ArduinoJson/src python3 devices/HAS1_generator/tests/run_http_body_tests.py
```

The audio suite compiles the actual vendored DFPlayer driver with a controlled
UART stream. It tests missing ACKs together with retained finish/error/timeout
events, bounded noise parsing, rollover, and recovery. This complements the
higher-level wire harness, whose audio calls are doubles.

The HTTP suite exercises the production bounded reader and actual HAS2_Wifi
request/poll functions with controlled streams and ArduinoJson 7.4.3. It checks
truncated/oversized/slow bodies, chunked framing, close-delimited responses,
cleanup, and suppression of stale callbacks after rejected responses. These are
host fault-injection checks; actual ESP32 UART/Wi-Fi timing and field reset
causation still require hardware logs. See [reset guards](../RESET_GUARDS.md).

## Bounded LED transmission

```sh
python3 devices/HAS1_generator/tests/run_neopixel_tests.py
```

This compiles the actual `generator_neopixel_tx.h` sender with controlled IDF
functions under AddressSanitizer/UndefinedBehaviorSanitizer. Eight groups cover
WS2812 MSB-first byte/timing preservation and 300 us low reset/latch intervals,
lost and late completion, a full transmit queue, other completion errors,
independent strips, clock rollover, invalid input, and partial initialization
failure. The mock deliberately provides no disable/delete API: timeout handling
must retain the potentially active payload and never enter unbounded cleanup.

A failed strip is disabled until the next boot; the other strips and main loop
continue. This is not an automatic retry mechanism. Tests verify the wrapper's
response to controlled IDF results, not physical RMT interrupts or LED waveforms.
The pinned ESP32 core build and a board smoke test are separate validation steps.

Diagnostic journal and Telnet integration (no hardware):

```sh
python3 devices/HAS1_generator/tests/run_diagnostic_tests.py
python3 devices/HAS1_generator/tests/run_telnet_tests.py
```

These execute the production journal/logger with ASan/UBSan and fake reset/time:
valid warm reset, power-on/brownout rejection, corrupted/torn/overwritten records,
sequence and millis wrap, previous-boot snapshot preservation, phase/kind replay,
fast HTTP failure visibility, bounded flood/truncation, UART bypass and incremental
replay larger than the control buffer. Wire behavior tests also reject diagnostic
calls made while the input lock is held. See `../DIAGNOSTICS.md` for log meanings.
