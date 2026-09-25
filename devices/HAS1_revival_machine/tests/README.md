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

The 53 control cases retain the existing gameplay scenarios, unknown-read latch
preservation and unavailable-reader coverage, and add maintenance integration:

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
  a held tag stays latched. Same-tag rearming requires at least two validated
  no-target sweeps spanning `RFID_REARM_ABSENT_MS` (currently 400 ms). Payload or
  transport errors, budget exhaustion, and unavailable-reader results interrupt
  the absence window. A different
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


## Transport and recovery tests

```sh
python3 devices/HAS1_revival_machine/tests/run_pn532_transport_tests.py
python3 devices/HAS1_revival_machine/tests/run_rfid_recovery_tests.py
python3 devices/HAS1_revival_machine/tests/run_rfid_runtime_trace_tests.py
python3 devices/HAS1_revival_machine/tests/run_rfid_diagnostic_tests.py
python3 devices/HAS1_revival_machine/scripts/test_rfid_bench.py
```

- **80 transport cases** compile the actual `pn532_transport.cpp` against SPI-byte
  and clock fakes with AddressSanitizer and UndefinedBehaviorSanitizer. They
  verify complete ACK/response consumption, frame and checksum rejection,
  no-target versus page/status errors, `0x05`/`0x0C` fail-fast handling, SPI API
  failures, same-CS wake/SAM and abort, 10-byte UIDs, cumulative ACK/response
  deadlines, and 32-bit clock wrap. The fake fails immediately if firmware reads
  past the queued FIFO bytes or leaves CS asserted.
  Card maintenance cases additionally check 16-byte READ, raw InCommunicateThru
  GET_VERSION (avoiding MIFARE Authentication A's `0x60` opcode), restricted
  WRITE addresses, and rejected/uncertain WRITE responses.
  NT3H1101 cases use its documented contiguous UID layout and cover sector
  selection, CRC restoration, live-session reads, interrupted operations,
  recovery, and bounded failure diagnostics.
- **25 sensor/recovery cases** compile the current sensor implementation plus
  actual absence-observation functions. They cover each gain only once, gain
  commit only after success, RF OFF/ON failures, the shared scan budget, three
  recovery attempts with 1/5/30-second scheduling and final lockout, recovery
  deferral during approval, preserved tag identity, interrupted absence windows,
  and deferred USB-only health logging. Actual `RfidInit()` verifies that the
  existing startup failure alert is sent exactly once, with no server-state
  writes from runtime faults or recovery attempts. The normal timed absence fixture uses
  three 80-ms target reads and 3-ms configuration operations: 264 ms including
  two 6-ms settling waits. A 441-ms clean sweep safely skips an optional RF cycle
  without relabeling validated absence as a transport failure. Budget checks
  permit less than one additional millisecond for the `millis()` clock's
  quantization; no real RF or scheduler latency is inferred from these fakes.
- **24 runtime trace cases** compile trace-off and trace-on separately, compare
  complete adapter call/result transcripts, and validate emitted JSON. Actual
  `loop()`, gameplay, ready-admin, pending-admin, debounce and unavailable-reader
  call paths are included. Existing trace bounds, wrap, phase timing, deferred
  formatting, previous-output overhead, and USB-only output checks remain.
- **Diagnostic tests** compile the same low-level sensor code and the actual
  command parser. They verify bounded inputs, stop/reset behavior, successful
  recovery after a failed gain change, unchanged gain on failure, 10-byte UID
  capture, trace capacity, and all 42 emitted JSON records. The diagnostic
  contract now rejects UID timeouts above 250 ms; the CLI's matching limits are
  independently checked among its 16 offline protocol/measurement tests.

The semantic adapter fake shared by sensor/trace/diagnostic tests is deliberately
separate from the byte-level transport fake. These suites verify code behavior;
they do not demonstrate physical recovery, antenna range, actual scan latency,
server latency, or a resolved field symptom. No test opens a serial port.

## NDEF and Telnet maintenance

```sh
python3 devices/HAS1_revival_machine/tests/run_card_ndef_tests.py
python3 devices/HAS1_revival_machine/tests/run_card_upload_tests.py
```

The portable encoder runs seven suites including exact URI/Text byte fixtures,
all offered prefix choices, page-7 alignment, UTF-8 and size validation, and
3,000 bounded randomized inputs with ASan/UBSan. The session suite executes the
production parser and writer against tag/NVS fakes; see
[58 engine cases and their limits](CARD_UPLOAD.md).

The control suite's maintenance cases use a component mock for the session's
gate decision, while exercising the actual main loop, state processing, approval
and relay code. They verify cancellation, normal/admin tag suppression, direct
state polling instead of legacy OTA/watchdog polling, blocked late open/OTA,
safe resumption, and no second RFID scan in the gate-release iteration. Actual
removal decisions, UID checks and card writes are covered by the session suite,
not claimed by the component mock. Runtime trace parity covers ordinary gameplay.
