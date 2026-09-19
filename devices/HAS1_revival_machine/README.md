# HAS1_revival_machine

## HAS3 BLE Beacon Build

HAS3 BLE advertising now fits in the default TTGO-T1 OTA partition. Keep the
existing partition scheme when updating already-deployed HAS1_revival_machine devices.

```text
Board = TTGO T1
Partition Scheme = default
```

Verified with ESP32 core 3.3.11 and the deployment workflow's library versions:

```text
Sketch uses 1278141 bytes.
Maximum default OTA app slot is 1310720 bytes.
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

`WIFI_POLL_INTERVAL_ACTIVATE_MS` is 2000ms (was 300ms). Since v49 the approval
wait polls `ReceiveMine()` directly every 300ms, so the idle activate poll no
longer drives the opening latency; it only picks up server-side state changes
such as a tagger blockade, which the device may now notice up to two seconds
late. The change is an experiment against PN532 reads that stalled only in
`activate` when a glove was held flat against the reader: the 300ms poll kept
the loop more than half busy with blocking HTTP and put every PN532 attempt
right after a Wi-Fi transmission. See `library_and_pin.h` for the reasoning
and the follow-up (gain pinning) if this does not help.

## Relay response and local timing

The first opening still requires the server to confirm `device_state="open"`.
Google Sheets debug logging has been removed, including its endpoint and HTTPS
requests. Approval now opens the relay before writing the local timing log.
The server checks, immediate approval refresh, and ghost-only reopening rules are
unchanged.

During gameplay, a recognized tag is latched so holding it against the reader
cannot repeat the role lookup, Situation request, or reopening pulse. The same
tag rearms after both gains fail to read a card at least twice over 600ms; a
single missed read does not rearm it. A different tag can be accepted once the
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
