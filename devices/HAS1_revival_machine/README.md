# HAS1_revival_machine

## HAS3 BLE Beacon Build

HAS3 BLE advertising now fits in the default TTGO-T1 OTA partition. Keep the
existing partition scheme when updating already-deployed HAS1_revival_machine devices.

```text
Board = TTGO T1
Partition Scheme = default
```

Current verified size:

```text
Sketch uses 1279965 bytes.
Maximum default OTA app slot is 1310720 bytes.
```

The BLE advertiser intentionally uses a minimal VHCI/HCI advertising path and
does not use Bluedroid GAP, scan, connect, GATT, notify, pairing, or BLE client
features.

The BLE local name keeps the server `device_name` unchanged after the `HAS3:`
prefix. For example, `TS1` advertises as `HAS3:TS1`; `BI1` advertises as
`HAS3:BI1`.
