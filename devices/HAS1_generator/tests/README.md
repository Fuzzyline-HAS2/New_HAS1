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
