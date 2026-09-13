# Duct host regression tests

Run from the repository root:

```sh
python3 devices/HAS1_duct/tests/run_cooldown_tests.py
```

Requires Python 3 and `clang++`; no third-party packages. Builds temporary C++ executables with strict warnings, AddressSanitizer and UndefinedBehaviorSanitizer.

- **23 device scenarios:** actual firmware state transitions, button logic and phrase builders, with queue submission recorded immediately. These check requested tracks, including a zero-second request; they do not claim those files exist or that hardware played them.
- **8 audio scenarios:** actual `audio_queue.ino`, phrase builders and measured `mp3_durations.h`, with only DFPlayer hardware replaced. Check track completion boundaries, whole-phrase FIFO, queue limits, repetition, missing files/hardware, unsigned clock rollover, language selection, obsolete requests, and concurrent door/cooldown timers.

The current media inventory lacks Korean `03/0000` and English `08/0002`. The actual scheduler suite verifies whole-phrase rejection for those missing tracks. Host clock rollover uses the host unsigned-long width; firmware compilation separately checks the ESP32 target. These tests cannot verify speaker output or SD-card contents on the physical device.
