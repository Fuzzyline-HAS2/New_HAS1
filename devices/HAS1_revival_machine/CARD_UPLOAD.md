# Telnet NTAG card writer

The server's `device_state = card-upload` enables a maintenance console on the
device's existing Telnet port 23. Connecting does not write a card. Each `write`
command arms exactly one job, which expires after 30 seconds. The job first
requires two clean no-card observations spanning at least 400 ms; then present
one card and keep it still until the result is printed.

## Game URL

Set `device_state` to `card-upload` on the server, connect to the device's IP
using Telnet, and enter:

```text
defaults
preview https://G1P1.p.fuzzyline.io
write https://G1P1.p.fuzzyline.io
```

The defaults are `format uri`, `prefix auto`, `layout game`, and
`template https://{code}.p.fuzzyline.io`. With these defaults, `write G1P1`
produces the same URL. `preview` shows content, padded user-memory size, actual
NDEF URI prefix identifier, and the four page-7 bytes without accessing a card.

The game layout keeps `G#P#` in page 7. Normal gameplay still performs its existing
single page-7 READ; it does not parse the complete URL or contact the website.
Game codes remain uppercase, single-digit `G0P0` through `G9P9`, matching the
existing reader. `MMMM` is not a supported game-writing value.

## Formats and prefixes

Commands and arguments are case-sensitive. Commands are terminated by CR or LF.

| Command | Effect |
| --- | --- |
| `help` | Print supported commands |
| `status` | Show mode, settings, pending job, and last result |
| `format uri` / `format text` | One URI record or UTF-8 Text record with language `en` |
| `prefix auto` | Choose the longest matching supported URI prefix |
| `prefix none` | Store the whole URI with identifier `0x00` |
| `prefix http://www.` | URI identifier `0x01` |
| `prefix https://www.` | URI identifier `0x02` |
| `prefix http://` | URI identifier `0x03` |
| `prefix https://` | URI identifier `0x04` |
| `template <pattern>` | Set a template containing exactly one `{code}` |
| `layout game` / `layout standard` | Require game compatibility, or write general NDEF |
| `preview <value>` | Validate and display the generated content without writing |
| `write <value>` | Arm one write and readback-verification job |
| `read` | Arm one read of UID and the first 144 user-memory bytes, printed as hex |
| `cancel` | Disarm the job; does not undo bytes already written |
| `save` | Persist validated settings locally in NVS |
| `defaults` | Restore session defaults; use `save` to persist them |

`write`, `read`, and `save` require server `card-upload` mode. Settings changes
are session-local until saved. A new connection reloads saved settings. Neither
NVS nor a reconnect restores a pending write or maintenance authorization.

URI templates expand only an exact `G#P#` input. A full URL or a bare hostname is
literal input. With a manual prefix, a bare hostname receives that prefix, while
an already complete URL must match it. An existing `www.` is not duplicated.
In `auto`/`none`, use a complete HTTP(S) URL when a web link is intended.
Text templates expand every input. Intermediate settings may be configured in
any order; `preview` and `write` enforce the resulting image's compatibility.

Example for an ordinary web link:

```text
layout standard
format uri
prefix https://www.
preview example.com
write example.com
```

Example for text:

```text
format text
layout standard
template {code}
preview TEST001
write TEST001
```

Game layout supports URI records whose stored URI body starts with the game
code. For example, the automatic prefix for `https://G1P1.p.fuzzyline.io` is
`0x04`, and for `https://www.G1P1.p.fuzzyline.io` it is `0x02`. Both can keep the
code at page 7, but they are different URLs. Uncompressed full HTTP(S) URLs,
codes later in the URL, and Text records cannot use this game layout. Use
`standard` for those; standard layout makes no game-compatibility guarantee.

## Card and failure handling

- Supported identification: NXP NTAG213/215/216 and original NT3H1101
  (NTAG I2C 1K, exact version `00 04 04 05 02 01 13 03`) GET_VERSION responses. Other
  products and incompatible version responses are refused before writing.
  NT3H1201 (2K) and NTAG I2C Plus are not included. The console prints `VERSION`
  and `MODEL` to make product mismatches visible.
- Only pages 4 through 39 (the common 144-byte user region) can be written.
  Encoded content is limited to 128 UTF-8 bytes, template to 96 bytes, and the
  complete padded image to 144 bytes. Larger NTAGs do not increase this limit.
- Before writing, the device checks the capability container, static/dynamic
  locks, password protection, mirroring, and the existing TLV layout. Custom
  memory/lock/proprietary layouts are refused. NTAG213's factory lock-control
  TLV is accepted and included in the new image; NTAG215/216 use reserved NULL
  TLVs. UID, CC, lock bits, password, and configuration pages are never written.
- NT3H1101 uses its own dynamic-lock page and live session-register checks;
  NTAG21x password/configuration offsets are not reused. Its default empty NDEF
  layout is accepted without inserting NTAG213 lock metadata. Before each user
  read/write, the device checks sector-3 session registers and returns to sector
  0. SRAM mirroring, pass-through, I2C ownership, and EEPROM busy/error states
  stop the job; writes additionally require RF writing enabled. An attached I2C
  host must remain idle throughout the job: RF cannot prevent it from changing
  session registers concurrently. These extra checks make I2C card jobs slower.
- The writer reselects the same UID before every operation. Card loss, another
  UID, mode changes, disconnect, cancellation, or timeout stops the job. It
  never automatically writes a replacement card or repeats a completed write.
- Page 7 is invalidated first. The image body is staged, its header restored,
  and page 7 committed last. Success requires same-UID readback matching the
  entire written image. A failed response after any write attempt is reported
  as `UNKNOWN`: inspect the card using `read` before deciding to retry.
- Multi-page writes are **not atomic** on RF loss or power loss. Cancellation
  cannot restore the old card. Keep one card stationary until verification
  finishes, and check the resulting URI with a phone before operational use.

## Leaving maintenance

Change the server device state to a normal safe state (`activate`, `tagger`,
`ready`, or `setting`) and remove the card. Gameplay and all opening paths,
including `MMMM`, stay blocked until removal is confirmed and the latest
observed device state is safe. Old pending approval identity is cleared on
entry. A late `open` does not open the relay; it requires a safe transition and
a subsequent new opening request. OTA transitions received while blocked are
also consumed: request OTA again after leaving maintenance normally.

Maintenance polls `ReceiveMine` directly to avoid the shared Wi-Fi library's
legacy OTA/watchdog actions during a write. Normal polling resumes after the
exit gate. The normal startup PN532 failure notification is unchanged.

This uses the existing unauthenticated LAN Telnet console. Server mode is the
write-enablement gate, not per-client authentication; enable it only for the
intended maintenance session. This change adds no website requests or external
card-data logging.

The normal firmware has been uploaded to Academy AR with unchanged bootloader,
partition table, and NVS. PN532 initialization and the Telnet maintenance mode
were verified. On 2026-09-25, the user supplied successful NT3H1101 read logs
before and after writing. The recorded URI decoded to
`https://G1P0.p.fuzzyline.io` (URI prefix `04`), and the user confirmed normal
operation and completion of testing. This verifies the tested Academy AR/card
combination; phone-specific recognition was not separately reported.

Protocol references: [NXP PN532 user manual](https://www.nxp.com/docs/en/user-guide/141520.pdf),
[NXP NTAG213/215/216 datasheet](https://www.nxp.com/docs/en/data-sheet/NTAG213_215_216.pdf),
[NXP NT3H1101/1201 datasheet, Rev. 3.3 (distributor copy)](https://www.mouser.com/datasheet/2/302/NT3H1101_1201-1127167.pdf),
[NDEF record representations](https://w3c-cg.github.io/web-nfc/#the-ndefrecord-interface).
