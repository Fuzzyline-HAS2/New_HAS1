# Beetle ESP32-C3

UART RX6/TX5, reset request input GPIO1 with pull-down, 115200 8N1. The TTGO owns
the active HIGH reset pulse. Beetle must see LOW for 50 ms after boot to arm; a
HIGH of at least 20 ms latches a request. After OTA is idle, the loop stops feeding
its own task-watchdog user and sets a 1 s panic timeout. The loop keeps yielding.
The core 3.3.11 C3 configuration enables panic reboot; verify a physical reboot
on the installed board as part of the first USB validation.

The user-confirmed schematic on 2026-09-17 connects TTGO GPIO12 to Beetle symbol
physical pin 4, labelled GPIO1. The previously installed v1 image monitored
GPIO3; its UART worked after battery power was connected, but reset pulses did
not change the boot ID. The GPIO1 correction was subsequently installed by USB.
A fresh baseline followed by the TTGO reset pulse produced a different boot ID
and a matching post-pulse PING/HELLO response, confirming reboot and UART recovery.
That image's UART protocol did not expose the hardware reset cause, so the test did not
directly read back a WDT reset reason. See [validation evidence](../docs/VALIDATION.md).

That hardware verification used the earlier v1 image. The new optional `LOG`
frames report `esp_reset_reason()` at boot and after each PING, plus reset-latch
and OTA phases. A compatible TTGO can show them through its USB/Telnet console
without opening the Beetle enclosure. The boot snapshot is deduplicated by boot
ID; other event sequences are checked independently. The first v1-to-new-image
OTA cannot expose v1's internal phases, but the new boot can report its cause.
The queue holds eight typed records, emits at most two per loop, and drops excess
diagnostics without changing heartbeat or OTA completion handling. No arbitrary
library logs, Wi-Fi credentials, keys, or HTTP contents cross UART. The exact
schema is documented in [IoTGloveProtocol](../../../libraries/IoTGloveProtocol/README.md).

BLE scanning is disabled until a valid MODE command. It stops after 6 s without a
valid TTGO command. The native NimBLE GAP callback streams only mapped `HAS3:`
names into a 24-entry queue. There is no dynamic BLEScan address-result map.
Candidates use a three-sample median, EMA, 6 dBm/750 ms switching hysteresis,
minimum -92 dBm, and a 5 s TTL. These are configurable starting values for field
calibration. Heartbeats and invalid location reports continue during outages.

Edit `beacon_map.h` with the exact device names broadcast by installed altar and
revival devices. The verified Origin server configuration has room aliases but
no device-prefix mapping. Default entries recognize room names themselves;
unmapped real device IDs deliberately report `unknown` until configured.

OTA uses the patched `first_store` HAS2_Wifi `TrySetup("badland")` API in a worker.
No `Setup()`/`Loop()` restart-on-Wi-Fi-failure path runs. The stock repository
build scripts stage that patch. A normal unsigned build has OTA disabled; copy
`secrets.h.example` to ignored `secrets.h` and supply the real deployment key for
signed OTA. The key is never sent over UART.

Firmware-only OTA requires the USB-installed partition layout to stay unchanged.
Partition OTA is intentionally disabled for this first version. The stored request
ID, source boot ID, source firmware, and requested target distinguish an applied
update from a failed/interrupted one.

`IG1|OTA|<requestId>|check\n` retains the fixed latest-release tag and SecureOTA
flow. `IG1|OTA|<requestId>|version|<N>\n` selects the immutable
`iotglove_beetle-v<N>` archive; N is canonical decimal 1 through 2147483647.
The shared pinned-release client verifies signed metadata for this board, exact
firmware, partition version, and image signature. A lower target version is an
explicit rollback. A same-version target skips only after metadata validation.
The current USB-installed partition version must match; no partition rewrite is
performed. Missing archives, bad metadata/signatures, and mismatched partitions
produce `failed` and stop the two-board update sequence.

For a pinned update, `updated` is reported only after a different boot ID is
running the exact requested version, which must differ from the source firmware.
The latest-release command retains its version-change completion rule. NVS keeps
the full request/result, so PING and identical request retries replay the result.
Reusing an ID with a different target is rejected. Older NVS records contain no
target/boot evidence: their IDs migrate as `failed`, requiring a fresh request ID.

Scanning pauses during OTA, the UART/main loop remains responsive, and reset-line
requests are deferred until flash work ends. A worker stuck over 180 s stops the
loop watchdog feed and triggers recovery within its 30 s timeout. BLE discovery
stuck over 8 s does the same. A watchdog reboot cannot guarantee recovery from a
hardware power fault; validate the GPIO12 boot strap pull-down on the TTGO.

Host regression tests cover the real UART parser, location tracker, reset latch,
and NVS OTA recovery policy (upgrade/rollback, wrong target, same boot, partition
mismatch, duplicate request, and legacy migration). Real BLE RF, GPIO pulses,
watchdog panics, signed OTA, and NVS power-loss
behavior still require the two physical boards.
