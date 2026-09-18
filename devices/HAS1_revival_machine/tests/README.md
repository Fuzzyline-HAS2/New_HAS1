# Revival machine host regression tests

Run from the repository root with Python 3.9+ and a C++17 compiler:

```sh
python3 devices/HAS1_revival_machine/tests/run_host_tests.py
```

The runner compiles production `game_state.ino` directly. It extracts the current
`CardChecking`, `Solenoid*`, and `NeoBlinkPurple` function bodies from `sensor.ino`,
avoiding unrelated PN532 and NeoPixel SDK dependencies. Pulse/poll constants are
also read from production. Generated files and the binary live in a temporary
directory and are removed after the run.

The 13 cases verify:

- An approved first opening reaches GPIO HIGH after the simulated 300 ms user
  lookup, 200 ms Situation request, 400 ms approval read, and 10 ms LED update.
  The captured local timing log reports 910 ms, includes the user lookup, and
  excludes the five-second relay pulse.
- HTTP 200 without server `open`, failed requests, and deferred approval retain
  their server approval requirements.
- Ghost-only reopening, `is_open` blocking, tagger blocking, administrator
  overrides, invalid tags, and repeated setting-mode pulses retain their behavior.
- Approval timeout clears pending timing state and logs locally without opening
  the relay.

These deterministic tests execute production control flow with fake clocks,
GPIO, local logging, and game-server responses. Real SDK compatibility and
physical relay/network timing still require the firmware build and device checks.
