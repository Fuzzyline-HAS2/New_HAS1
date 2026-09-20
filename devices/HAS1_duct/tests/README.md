# Duct host regression tests

Run from the repository root:

```sh
python3 devices/HAS1_duct/tests/run_cooldown_tests.py
```

Requires Python 3 and `clang++`; no third-party packages. Builds temporary C++ executables with strict warnings, AddressSanitizer and UndefinedBehaviorSanitizer.

- **32 device scenarios:** actual firmware state transitions, button logic, opening-line selection, server activate handling, server `left_time` blockade countdown and phrase builders, with queue submission recorded immediately. These check requested tracks, including a zero-second request; they do not claim those files exist or that hardware played them.
- **10 audio scenarios:** actual `audio_queue.ino`, phrase builders and measured `mp3_durations.h`, with only DFPlayer hardware replaced. Check V2 opening and blockade track completion boundaries, whole-phrase FIFO, queue limits, repetition, missing files/hardware, unsigned clock rollover, language selection, obsolete requests, and concurrent door/cooldown timers.

The duration table targets `HAS2-Nextion` branch `feat/audio-library-v2`, `audios/V2/duct_MP3_22k/01~08` plus the opening lines `09/0712`, `09/0719`, `10/0712`, `10/0719`; its source commit is recorded in the header. Durations are WAV frame counts divided by the sample rate, rounded up to milliseconds. The scheduler adds its `MP3_TRACK_MARGIN_MS` (100ms) margin separately.

The current media inventory lacks Korean `03/0000` and English `08/0002`. The actual scheduler suite verifies whole-phrase rejection for those missing tracks. Host clock rollover uses the host unsigned-long width; firmware compilation separately checks the ESP32 target. These tests cannot verify speaker output or SD-card contents on the physical device.
