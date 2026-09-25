# Card-upload session contract

`CardUploadSyncMode(device_state)` returns true only for `card-upload`. Both block
queries remain true while active and after leaving until a **currently safe**
state (`activate`, `tagger`, `ready`, or `setting`) and a clean removal observation
have both been established. `open`, `github`, unknown, and null states are not
safe. Removal requires at least two validated no-target observations spanning
400 ms; errors interrupt that window. The block persists without a Telnet client.

The root sketch supplies `CardUploadOutput(line)`, a Telnet-only line writer,
`RfidUploadSelect(uid, length)`, and `RfidUploadObserve(result)`. Selection already
handles reader health, so its results are never observed again by this module.
Only direct PN532 version/read/write results are passed to `RfidUploadObserve`.
Call `CardUploadLoop()` once per main-loop iteration. It performs at most one
selection or one PN532 command stage per call, with each direct command limited
to 100 ms, except the NT3H1101 session inspection (250 ms shared across its
sector selection, register access, restoration, and sector-0 header check).
A complete job has a 30-second deadline.

Connection loads saved settings and never resumes a job. Settings validation is
structural: intermediate combinations are allowed; `preview` and `write` validate
whether the chosen input can actually be encoded. `read`, `write`, and `save`
require active server mode. All jobs require removal followed by a fresh tag.
Settings changes require an idle job; `cancel`, `status`, and `preview` remain
available. NVS namespace `revival-card`, key `config`, stores a versioned 105-byte
settings blob only. Pending data, UID, mode, and jobs are never saved.

Writes require a supported NTAG213/215/216 version response, compatible readable
CC with at least 144 bytes, writable CC, zero static/dynamic lock bytes, disabled
password protection, default ACCESS flags, and no active mirror. Configuration
inspection reads the last user page through CFG1, excluding PWD/PACK. Custom TLVs
are refused; the exact NTAG213 factory lock TLV is accepted and preserved. The
write range is limited to pages 4–39. No lock, CC, configuration, password, or
manufacturer page is written.

Original NT3H1101 is a separate exact-version profile, with factory CC size
`6D`, dynamic-lock READ at sector-0 `E2`, and no NTAG21x AUTH0/ACCESS checks.
Its memory header holds all seven UID bytes contiguously at offsets 0–6,
followed by SAK/ATQA; it does not use the NTAG21x interleaved BCC layout.
Each user-memory operation is preceded by live sector-3 session inspection,
which returns to sector 0 before the data operation on the same selection.
NC_REG must have pass-through/mirroring off and, for writes, TRANSFER_DIR set.
NS_REG must report an RF field and no I2C ownership or EEPROM busy/error; its
reserved final byte must be zero. I2C Plus/2K and custom CC sizes stay refused.
The transport disables only RxCRCEn to receive the four-bit sector ACK, then
restores and reads back RxMode. Sector packet-2 success requires a validated
PN532 RF-timeout status, never a host timeout. It compares the sector-0 UID/CC
header before publishing the session bytes. Failures block all card APIs until
SAM initialization repairs CRC and cycles the RF field; cold startup also
normalizes these settings.

The external I2C host must remain idle because session-register writes cannot
be locked out by RF. Default NULL padding is used, not NTAG213 lock metadata.

Each successive memory operation is preceded by same-UID selection (and, for
NT3H1101, the bounded session inspection). Page 7 is first
invalidated, page 4 temporarily exposes empty NDEF, body pages are staged, page 4
is committed, and page 7 is committed last. All programmed bytes are then read
back. Standard layouts also overwrite page 7, so an old game code is not retained
outside a shorter replacement image. Successful completion disarms the job; a
held card cannot trigger repetition. This sequence is **not atomic** under RF or
power loss. Any error, cancellation, disconnect, or mode change after a write
attempt yields `UNKNOWN`, never “unchanged.” It requires inspection/manual retry.
The `read` command prints UID and the first 144 user-memory bytes as hex.

Run the actual engine and encoder with deterministic NVS/card fakes:

```sh
python3 devices/HAS1_revival_machine/tests/run_card_upload_tests.py
```

The 58 cases run with ASan/UBSan and cover supported models, factory metadata,
short standard and long literal writes, commit order, readback, UID changes,
uncertain writes, interruption, deadlines/wrap, locking/protection refusal,
mode/exit gates, Telnet overflow/IAC/CRLF/backspace, and settings persistence.
NT3H1101 cases additionally cover exact model matching, read/write, live-session
changes after the first write, lock/config refusals, session failures, and
preservation of uncertainty after a write attempt.
They never open a serial port and cannot validate RF reliability, EEPROM behavior,
actual card authenticity, or physical timing. Hardware testing remains separate.
