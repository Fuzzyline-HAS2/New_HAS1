# HAS1 BLE beacon

Arduino ESP32 **3.3.11** library for the duct, generator, escape main, and tag
machine main. It owns the controller's VHCI interface exclusively. Do not combine
it with another BLE/Bluetooth host stack or the old advertiser in one firmware.

```cpp
#include <HAS1BleBeacon.h>

// On a server device_name update, in the setup/loop task:
Has1BleBeacon::setDeviceName(deviceName);

// Once in setup, after peripheral/Wi-Fi initialization and before actuators
// start or safety-critical elapsed-time deadlines are armed:
Has1BleBeacon::begin();

// At the end of loop; false defers all new commands while the device is busy:
Has1BleBeacon::poll(allowBleWork);
```

Names are copied immediately and may be set before `begin()`. Valid names contain
1–18 ASCII letters, digits, `_`, or `-`. Invalid, null, or empty names request an
asynchronous stop. Callers should skip absent server fields if the existing name
must be retained. Configuration snapshots one name and coalesces newer updates.
Before sending Enable, it checks that the snapshot is still current. An Enable
already sent can finish before a newer name is applied; the next permitted
configuration pass disables that advertisement and applies the latest name.

Advertising matches the existing receiver: `HAS3:<device_name>`, scannable and
non-connectable legacy advertising, 400 interval units (250 ms), all three
advertising channels, flags `0x06`, and the existing TX power data byte `3`.
The TX power field is compatibility metadata; this library does not calibrate or
change physical controller transmit power. The complete local name is present
in both advertising and scan response data. No game-state restriction is added.

## Scheduling and errors

`begin()` starts the controller in BLE-only mode once. This boot operation may
block inside Arduino controller initialization. Failure leaves BLE unavailable
until reboot. A strong `bleInUse()` retains BLE memory while allowing Arduino to
release Classic memory before setup. It requires the 3.3.11 Arduino API.

`setDeviceName()` and `poll()` allocate no heap, start no task, and contain no
sleep or wait loops. `poll()` sends at most one HCI command and checks send-ready
before sending. It still runs as part of the device loop; the Espressif controller
and Wi-Fi own internal tasks and share radio resources. This is not a guarantee
of zero radio interference or zero time spent inside the send API.

`poll(false)` consumes completed results and checks outstanding-command timeouts
but sends no commands, including disable commands. Existing advertising continues.
Busy time does not spend the send-readiness budget. BLE response processing and
startup/reconfiguration therefore can pause while higher-priority device work
is active. All public functions belong to the same setup/loop task; only the VHCI
callback crosses tasks, through a fixed-size mailbox guarded by ESP-IDF
`portMUX_TYPE` and `portENTER_CRITICAL_SAFE` / `portEXIT_CRITICAL_SAFE`. These
supported primitives handle task or ISR entry and synchronize both ESP32 cores.
Each critical section contains only a few primitive field copies; it can briefly
contend with the other core, but contains no controller calls, time reads, parser,
or state-machine work. HCI send is always outside the critical section.

Send-readiness failure after 50 ms and an acknowledged HCI error use a 2-second
backoff, then a controller HCI Reset before reconfiguration. They do not restart
the controller. No command failure advances the successful configuration sequence.
A successful Command Status is not a Command Complete and cannot advance it.

A missing Command Complete after 500 ms retains ownership of that command and
suspends BLE commands. HCI has no transaction ID, so resending the same opcode
would risk accepting the old response for a new command. If the matching late
response arrives, the library backs off and resets before trying again. If it
never arrives, device/Wi-Fi work continues, but BLE needs a reboot to recover.
The actual radio state can then be unknown, and previously enabled advertising
may continue; the library cannot guarantee it stopped without acknowledgment.

Response time is stamped in the callback. A timely acknowledgment consumed by a
busy device loop several seconds later does not falsely become an HCI timeout.
Unsigned elapsed-time comparisons support `millis()` rollover.

`diagnostics()` returns fixed-size state, acknowledged/unknown radio status,
last error/HCI status, outstanding opcode, timeout flag, and command/failure
counts. There is no default Serial logging, avoiding device UART interference.

## Connection-time diagnostics

The four device integrations send one `[diagnostics]` snapshot when a Telnet
client connects. It reports firmware version, device name, Wi-Fi MAC/local IP,
`heap_free`, `heap_min`, `heap_largest`, and BLE state/radio/error/HCI status,
pending opcode, completion flags, command count, and failure count. The snapshot
uses a 384-byte stack buffer, limits the device name to 18 characters, and creates
no dynamic `String`. Formatting the maximum field widths produces 315 bytes.

The sender validates the socket descriptor, then attempts exactly one
`send(..., MSG_DONTWAIT)`. A failed or partial send is dropped without waiting or
retrying; reconnect to request another snapshot. This does not add periodic
logging or change the existing console mirroring path.

For a settled, successfully configured advertisement, `ble_step=7` means
`Running`, `ble_radio=2` means acknowledged `On`, and `ble_pending=0` means no
outstanding HCI command. `ble_error=0` means `None`. These values describe the
controller's acknowledged state, not proof that another device received the
advertisement. Compare the firmware version/name/MAC/IP with the intended OTA
target, and use an external BLE receiver for radio evidence.

## Host verification

`BeaconState.cpp` has no Arduino dependencies. Host tests can drive `StateMachine`
with a fake clock, send availability, command results, and name changes. The H4
event parser is shared by the host tests and hardware adapter. Actual Wi-Fi
latency, controller behavior, runtime free heap, and device timing still require
hardware verification.

## Arduino library selection

Arduino does not automatically discover this repository's `libraries/` folder.
Install `HAS1BleBeacon` into the Arduino sketchbook libraries folder, or pass it
explicitly to Arduino CLI from the repository root (along with any other local
library paths required by the selected device):

```sh
has1_target=HAS1_generator
has1_fqbn="$(python3 -c 'import sys; sys.path.insert(0, "scripts"); from firmware_targets import TARGETS; print(TARGETS[sys.argv[1]][1])' "$has1_target")"
arduino-cli compile --fqbn "$has1_fqbn" \
  --library ./libraries/HAS1BleBeacon \
  --library ./libraries/HAS2_Wifi \
  "./devices/$has1_target"
```

Set `has1_target` to `HAS1_duct`, `HAS1_generator`, `HAS1_escape_main`, or
`HAS1_tagmachine_main`. The command reads the verified board options and partition
scheme from `scripts/firmware_targets.py`; generator/duct use `min_spiffs`, while
escape/tag main use `default`. Use Arduino ESP32 core 3.3.11 and the device's
required installed dependencies.

See [validation results and hardware test checklist](VALIDATION.md) for this
integration's build sizes, automated checks, and remaining physical tests.
