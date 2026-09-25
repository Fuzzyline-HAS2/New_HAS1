# HAS1_tagmachine_sub
HAS1_tagmachine_sub beetle 코드

## TTGO UART protocol (9600 bps)

Control frames stay one character long so older TTGO firmware, which treats only
four-or-more-character frames as tag data, safely ignores new status frames.

- `W`: Beetle boot hello; repeated once per second until the TTGO replies `W`.
- `H`: Beetle MCU heartbeat. It is followed by the current `A`/`E` status. A
  TTGO may also send `H` as a probe and receives the same response.
- `A`: PN532 is initialized and ready.
- `E`: PN532 is unavailable; the Beetle continues bounded backoff retries while
  still servicing UART.
- `R` from TTGO: force PN532 reinitialization. Beetle replies `R` only after the
  requested recovery succeeds, then sends `A`.

PN532 recovery is the primary mechanism. Initialization validates firmware,
SAM configuration, finite passive-target retries, and gain configuration. Failed
attempts back off from 1 to 10 seconds without blocking UART. A 12-second task
watchdog is only a last resort for a library/driver deadlock.

Run the host-side recovery policy tests with:

```sh
python3 devices/HAS1_tagmachine_sub/tests/run_tests.py
```
