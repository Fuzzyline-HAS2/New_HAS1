# HAS1_revival_machine

## Telnet card writer

Set server `device_state` to `card-upload` to enable one-shot NTAG URI/Text
writing. See [commands, game compatibility, and maintenance exit](CARD_UPLOAD.md).
The writer supports automatic/manual URI prefixes, templates, preview, saved
settings, and same-UID readback verification. NT3H1101 read/write and normal
operation were confirmed on Academy AR on 2026-09-25; see the test scope in
the linked guide.

## HAS3 BLE Beacon Build

HAS3 BLE advertising now fits in the default TTGO-T1 OTA partition. Keep the
existing partition scheme when updating already-deployed HAS1_revival_machine devices.

```text
Board = TTGO T1
Partition Scheme = default
```

Verified with ESP32 core 3.3.11 and the deployment workflow's library versions:

```text
Normal bench firmware with the Telnet card writer uses 1299265 bytes (2026-09-25).
Maximum default OTA app slot is 1310720 bytes.
Remaining program space is 11455 bytes (11.2 KiB).
```

The BLE advertiser intentionally uses a minimal VHCI/HCI advertising path and
does not use Bluedroid GAP, scan, connect, GATT, notify, pairing, or BLE client
features.

The BLE local name keeps the server `device_name` unchanged after the `HAS3:`
prefix. For example, `TS1` advertises as `HAS3:TS1`; `BI1` advertises as
`HAS3:BI1`.

## Device mode and server polling

The LED colour, Wi-Fi poll interval and tag enable are a function of the
(`game_state`, `device_state`) pair. `DataChange()` re-derives them whenever
either field changes, not only on the transition of one field. This fixes two
cases the field-by-field edge triggers got wrong: `ready -> activate` while
`device_state` was already `"activate"` stayed red with the slow poll, and a
`device_state` re-arm arriving during `ready` painted the ready red yellow.
One-shot actions (the opening pulse, the `is_open` write, the OTA check) remain
tied to a `device_state` transition only, so a `game_state` change after an
opening never re-energises the relay.

`WIFI_POLL_INTERVAL_ACTIVATE_MS` stays at 300ms. A 2000ms experiment (v51) did
not change the flat-held-glove read failure and only delayed the device's view of
server-side changes such as a tagger blockade, so it was reverted.

## PN532 transactions and bounded recovery

The device-local `pn532_transport` adapter uses the existing software SPI pins
and Adafruit BusIO dependency. Each transaction validates the complete ACK and
response, including command, frame length, checksum, and postamble. Response
length determines exactly how many bytes are read while CS stays asserted;
short no-target responses are not overread, and longer UID responses are not
left partially unread. RF configuration responses are consumed and validated
before the next command.

A successful UID detection sends one NTAG READ for page 7 and returns its first
four data bytes (`G#P#` or `MMMM`). The READ command itself returns four pages;
consuming that complete PN532 response does not issue additional card reads.
Normal gameplay does not write tag data; writing requires card-upload mode.

Only a validated response reporting zero targets is a clean absence. A tag
exchange error, invalid frame, SPI status error, deadline, or unavailable reader
is not absence. These failures preserve the held-tag latch and reset its pending
absence interval, so a communication fault cannot rearm the same tag.

The gain sweep tries each of CONTACT (18 dB), NEAR (23 dB), and FAR (33 dB) at most
once. Software gain changes only after a validated configuration response.
Configuration/transport failures stop the sweep immediately. ACK and response
waits share a command deadline; command deadlines also fit inside the overall
scan deadline. The limits are 250ms for target detection, 100ms for configuration
and other commands, and 450ms for a scan or recovery attempt. RF cycling includes
an explicit 6ms off interval and an activation guard after field-on. These limits
cover the reader work, not synchronous server requests or the relay pulse.

A reader fault invalidates the known configuration. Automatic recovery is limited
to three attempts, after 1s, 5s, and 30s backoffs. Recovery does not run while
server approval is pending. Exhaustion leaves the reader unavailable until an
explicit reset/restart; it does not create a repeated multi-second scan loop.
Recovery validates the firmware response and all required configuration before
accepting tags again. The existing wiring has no PN532 reset-control pin:
host ACK abort, SPI wakeup, and restarting the ESP32 are not PN532 hardware resets.
A PN532 that remains unresponsive requires its actual power/reset path.

The September 25 bench investigation observed SPI status `0x05` (READY plus
receive overrun), invalid ACK data, and failed reinitialization. Testing only
READY bit 0 did not restore communication, so exception bits are treated as faults.
The initiating event and its relationship to the reported near/far symptom remain
unproven. The gain modes are retained for RF coverage; this change does not claim
that every distance-related failure is a transport fault.

Protocol references: [NXP firmware manual](https://www.nxp.com/docs/en/user-guide/141520.pdf)
(frame exchange, SPI wakeup, and RFConfiguration),
[PN532/C1 datasheet, SPIstatus table 125](https://www.nxp.com/docs/en/nxp/data-sheets/PN532_C1.pdf#page=83),
and [NXP RF polling timing guidance](https://www.nxp.com/docs/en/application-note/AN12057.pdf#page=5).
See [RFID bench instructions](scripts/RFID_BENCH.md) for isolated diagnostics,
original-firmware backup/restore requirements, and measurement limits.

## Relay response and local timing

The first opening still requires the server to confirm `device_state="open"`.
Google Sheets debug logging has been removed, including its endpoint and HTTPS
requests. Approval now opens the relay before writing the local timing log.
The server checks, immediate approval refresh, and ghost-only reopening rules are
unchanged.

During gameplay, a recognized tag is latched so holding it against the reader
cannot repeat the role lookup, Situation request, or reopening pulse. The same
tag rearms after at least two clean, complete no-target scans spanning the
400ms absence interval. A single miss or reader fault does not rearm it. A different tag can be accepted once the
previous request is resolved. Setting tags and `MMMM` administrator cards retain
their existing behavior.

While the first opening awaits approval, normal tags are ignored and the main
loop prioritizes a direct `ReceiveMine()` every 300ms after the preceding query
finishes. This bypasses the `Loop`/`shift_machine` gate and preserves the first
tag's identity and timing. Regular PN532 detection and gain-switch retries are
skipped while waiting. An administrator-only probe still runs at the current
gain no more than once per second, and never on a loop iteration that performed
an approval query. The probe's command waits are still synchronous.

An HTTP send failure, game/device state change, or 15-second deadline clears the
approval wait. The deadline is checked in the main loop even when the server's
shift flag never changes; an in-flight synchronous HTTP operation must still
return before the loop can enforce it. Known non-ghost tags keep their existing
Situation event and immediate refresh, then release the wait so they cannot
block the next ghost. `Situation()` only exposes HTTP success, so a rejected
ghost request with unchanged device state expires at the deadline. Clearing a
wait does not rearm a held tag, and it never opens the relay without a server
`open` state. After an uncertain timeout or ghost-request HTTP failure, the
original user's identity is retained for a late approval's `is_open` write;
a subsequent game/device cancellation clears it. The timing measurement ends
at timeout/failure and is not completed by that late opening.

The `[GhostTiming] RELAY ON` elapsed time starts when a valid `G#P#` tag is
recognized, before the role lookup, and ends at the actual relay GPIO HIGH
timestamp returned by `SolenoidPulse`.
The approved opening still holds the relay for five seconds and turns it off
before logging or sending the glove's `is_open` update. The console timing line
appears after that pulse but excludes its five seconds, and includes the separate
`role_receive` and `situation` durations. Admin/setting/reopening pulses do not
complete a pending first-open measurement. The local timeout message reports an
approval wait reaching 15 seconds; it does not represent a relay opening.

Diagnostics are available through the existing USB Serial/Telnet console only.
No background task, queue, or external logging service is needed for these logs.
Server/Wi-Fi latency, the role lookup, existing RFID polling, and the five-second
pulse still apply. Compare the local relay-on elapsed time on hardware before and
after the change to measure the remaining server and radio latency.

The [host regression tests](tests/README.md) exercise the production approval and
relay control paths, local timing, and the absence of Sheets logging.
