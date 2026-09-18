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
Sketch uses 1275657 bytes.
Maximum default OTA app slot is 1310720 bytes.
```

The BLE advertiser intentionally uses a minimal VHCI/HCI advertising path and
does not use Bluedroid GAP, scan, connect, GATT, notify, pairing, or BLE client
features.

The BLE local name keeps the server `device_name` unchanged after the `HAS3:`
prefix. For example, `TS1` advertises as `HAS3:TS1`; `BI1` advertises as
`HAS3:BI1`.

## Relay response and local timing

The first opening still requires the server to confirm `device_state="open"`.
Google Sheets debug logging has been removed, including its endpoint and HTTPS
requests. Approval now opens the relay before writing the local timing log.
The server checks, immediate approval refresh, and ghost-only reopening rules are
unchanged.

The `[GhostTiming] RELAY ON` elapsed time starts when a valid `G#P#` tag is
recognized, before the role lookup, and ends at the actual relay GPIO HIGH
timestamp returned by `SolenoidPulse`.
The approved opening still holds the relay for five seconds and turns it off
before logging or sending the glove's `is_open` update. The console timing line
appears after that pulse but excludes its five seconds, and includes the separate
`role_receive` and `situation` durations. Admin/setting/reopening pulses do not
complete a pending first-open measurement. The local timeout message reports an
approval wait exceeding 15 seconds; it does not represent a relay opening.

Diagnostics are available through the existing USB Serial/Telnet console only.
No background task, queue, or external logging service is needed for these logs.
Server/Wi-Fi latency, the role lookup, existing RFID polling, and the five-second
pulse still apply. Compare the local relay-on elapsed time on hardware before and
after the change to measure the remaining server and radio latency.

The [host regression tests](tests/README.md) exercise the production approval and
relay control paths, local timing, and the absence of Sheets logging.
