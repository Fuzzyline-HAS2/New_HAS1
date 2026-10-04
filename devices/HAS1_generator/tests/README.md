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
[Telnet logging](../TELNET_LOGGING.md) for capture scope and limits. Both generator
harnesses run in the `Generator tests` pull-request workflow.
