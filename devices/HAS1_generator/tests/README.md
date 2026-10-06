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
```

The host harness exercises the production RAM history and bounded sender with
AddressSanitizer/UndefinedBehaviorSanitizer: partial sends, replay/live ordering,
backpressure, overwritten history, per-loop send limits, disconnects, ESP-IDF
message truncation, and preservation of the original reset report. It does not
simulate FreeRTOS concurrency or a physical Wi-Fi link. See
[Telnet logging](../TELNET_LOGGING.md) for capture scope and limits. The generator
harnesses run in the `Generator tests` pull-request workflow.

## Absolute wire synchronization regression checks

Use ArduinoJson **v7.4.3** headers, matching the firmware build:

```sh
ARDUINOJSON_INCLUDE=/path/to/ArduinoJson/src python3 devices/HAS1_generator/tests/run_wire_tests.py
```

The ASan/UBSan harness compiles production `wire.ino`, the state header, and
`wire_http.ino` against fake GPIO/server/HTTP streams, a deterministic task clock,
and the actual ArduinoJson parser. It drives the production static sampler task
with fake FreeRTOS primitives, including samples while synchronous HTTP advances
the clock. Checks cover the 100 ms sampling interval, 1-second stable window,
250 ms freshness limit, stopped/failed samplers, and a new stable window after
main observes a mismatch without refreshing the sampler heartbeat. Critical-section
checks keep fake logging, rendering, audio, and HTTP outside the state lock.

Other cases cover short bounce, 0–4 counts, LG's 3-pack threshold, lost ACKs,
latest-value retries, the 2-second failure backoff, and successful updates without
a blanket 2-second gate. CAS fencing, new rounds/identity/epoch, preserved repaired
state, strict response validation, and response size/deadline limits remain covered.
The actual completion functions are also compiled with fake audio and HTTP to
check that battery completion yields while audio is pending, rechecks readiness
after audio and the state send, and does not repeat its announcement or send.

These deterministic checks do not establish real FreeRTOS concurrency, task
latency, a real network, DFPlayer UART timing, or electrical noise behavior. Field
hardware validation has not been performed for this change, and the harness does
not guarantee a fixed connection-to-completion time. See
[the wire protocol and deployment sequence](../WIRE_SYNC.md). The Generator tests
workflow fetches the pinned JSON headers and runs this harness.
