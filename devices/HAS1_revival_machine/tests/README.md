# Revival machine host regression tests

Run from the repository root with Python 3.9+ and a C++17 compiler:

```sh
python3 devices/HAS1_revival_machine/tests/run_host_tests.py
```

The runner compiles production `approval.ino`, `game_state.ino`, and `timer.ino`
directly. It extracts the current main `loop`, RFID dispatch functions,
`CardChecking`, `Solenoid*`, and `NeoBlinkPurple` bodies from their production
files. Relay, polling, timeout, and RFID constants also come from production.
Generated includes and the binary live in a temporary directory and are removed
after the run.

The 35 cases verify:

- The device mode (LED colour, poll interval, tag enable) is re-derived from the
  (`game_state`, `device_state`) pair whenever either field changes. `ready ->
  activate` with `device_state` already `"activate"` turns yellow and arms the
  activate poll; a `device_state` re-arm during `ready` leaves the ready red in
  place; a `game_state` change after an opening never re-pulses the relay or
  rewrites `is_open`.

- An approved first opening reaches GPIO HIGH after the simulated 300 ms user
  lookup, 200 ms Situation request, 400 ms approval read, and 10 ms LED update.
  The local timing log reports 910 ms, includes the user lookup, and excludes
  the five-second relay pulse.
- A held game tag submits one user lookup and one Situation. Additional same or
  different tags cannot replace the first pending request's identity or timing.
  An HTTP 200 response without `open` never actuates the relay.
- Deferred approvals use direct `ReceiveMine` polling. Ordinary server `Loop`
  calls and the full PN532 gain-switch read path stay idle during approval waits;
  an iteration that performs approval polling does not also probe PN532.
- A separate, rate-limited single-gain probe still accepts an administrator card
  while pending, without sending a new game request.
- Failed requests and independent approval timeouts release the busy state while
  a held tag stays latched. Same-tag rearming requires at least two failed full
  reads spanning 600 ms; a transient failed read does not rearm it. A different
  tag can be processed after the prior wait ends.
- Ordinary timer polling resumes without accumulated catch-up HTTP requests.
  Timeout arithmetic also works across the host unsigned-clock wrap boundary.
- A denied non-ghost tag does not prevent a different ghost from requesting an
  opening immediately. Known game/device-state cancellation ends pending work.
- An authoritative `open` arriving after an uncertain HTTP failure or timeout
  still marks the original glove as used. A later game-state cancellation clears
  that attribution before an unrelated `open` can consume it.
- Ghost-only reopening after removal, `is_open` blocking, tagger blocking,
  administrator overrides (including ready polling), invalid tags, and repeated
  setting-mode pulses retain their behavior.

These deterministic tests execute production control flow with fake clocks,
GPIO, local logging, timer callbacks, PN532 read outcomes, and game-server
responses. The timer fake preserves SimpleTimer's cumulative interval scheduling
so polling backlog behavior is exercised. They do not reproduce RF conditions,
actual PN532 blocking durations, HTTP reconnection, or hardware relay timing;
firmware compilation and physical device checks remain necessary.
