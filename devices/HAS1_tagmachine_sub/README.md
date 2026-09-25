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

## Signed OTA

An OTA-capable TTGO sends the compact, allocation-free protocol alongside the
legacy one-character protocol. Every Beetle response starts with `R`, which old
TTGO firmware already consumes as control rather than treating it as tag data.

- `Q:<request>`: query capability and recovery state. Beetle replies
  `RV:<request>:2:<firmware>:<partition>:<boot>` and then replays the last OTA
  outcome, if one exists.
- `U:<request>:<target>`: verify/install the exact immutable
  `HAS1_tagmachine_sub-v<target>` Release. Both physical Beetles receive the same
  target latched by the TTGO.
- `U:<request>`: compatibility mode; authenticate the fixed channel manifest,
  then pin its target before downloading any image.
- `RO:<request>:<code>:<firmware>:<partition>:<boot>`: reports an exact-request
  outcome. Codes are `A` accepted, `P` flashing, `U` updated, `S` skipped,
  `F` failed, `B` busy, and `D` disabled.

The UART carries only control and proof. A worker task connects to the normal
`badland` Wi-Fi selection, authenticates signed metadata for the exact board,
version and `default` partition, and streams `update.bin` from the immutable
version Release. The image is committed only after its HMAC-SHA256 matches the
authenticated metadata. Each transfer has idle and total deadlines. PN532
initialization and polling pause during OTA, while UART, status reporting and
the loop watchdog continue. A worker that remains busy for 180 seconds stops
receiving watchdog feeds; the existing 12-second watchdog then recovers it.

The request, requested/verified target, old firmware/boot ID, partition version
and accepted result are stored in NVS before the `accepted` UART acknowledgement
or any flash work.
After reboot, `updated` is reported only when a newer
firmware version is running under a new boot ID on the same partition schema.
Interrupted or ambiguous attempts recover as `failed`. Results are replayed on
`Q`, so a lost reboot frame does not cause a second flash.

It never changes the partition table or permits a downgrade. The current
`default` 4 MB layout has two 0x140000-byte OTA slots. A terminal pre-commit
failure can be retried by the TTGO with a new request ID, but only three board
attempts are allowed per sequence. The first protocol-v2 OTA baseline must be
installed once over USB on both Beetles; later versions can use OTA. Main and
Sub Beetle run this same sketch and release target.

## USB baseline auto-upload

The helper performs one clean build with the production HMAC key, waits for a
new physical USB serial port, and uploads at 460800 bps. Because the Beetle's
USB-UART adapter has a generic identity, disconnect all target Beetles before
starting and connect exactly one when prompted.

To program Main and Sub sequentially with the same image:

```sh
python3 devices/HAS1_tagmachine_sub/scripts/auto_usb_upload.py \
  --count 2 \
  --expected-version 4
```

Version 4 is the first OTA-capable Beetle baseline. This guard intentionally
refuses the current version-3 source until the release workflow has bumped and
published version 4, avoiding a different binary under the immutable v3 label.

After the first upload, physically disconnect that Beetle. The helper waits for
the disconnection before accepting the second one, preventing a reset or USB
re-enumeration from flashing the same device twice. An already-connected board
is deliberately ignored in automatic mode; select it explicitly only when its
port is known:

```sh
python3 devices/HAS1_tagmachine_sub/scripts/auto_usb_upload.py \
  --port /dev/cu.usbserial-XXXXXXXX \
  --expected-version 4
```

`secrets.h` must contain the same real `HMAC_SECRET` configured for release
publishing. The value is validated but never printed. When the key lives
outside this checkout, pass it without copying it into the repository:

```sh
python3 devices/HAS1_tagmachine_sub/scripts/auto_usb_upload.py \
  --count 2 \
  --expected-version 4 \
  --secret-file /absolute/path/to/secrets.h
```

The first run prepares pinned libraries in the ignored
`build/tagmachine-beetle-usb` cache. Use `--refresh-dependencies` to rebuild
that cache, `--expected-version N` to guard against flashing the wrong release,
or `--dry-run` to compile and check the OTA-slot margin without touching USB.
Actual uploads require `--expected-version`; update its value for later releases.
If automatic reset into the bootloader fails, hold BOOT, press and release RST,
release BOOT, then rerun the command; failed uploads are never retried silently.

Run the host-side recovery and persistent OTA record tests with:

```sh
python3 devices/HAS1_tagmachine_sub/tests/run_tests.py
```
