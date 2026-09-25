# IoTGloveProtocol v1

The TTGO and Beetle use the same allocation-free parser. Compile with the Arduino
library `IoTGloveProtocol`; no parent-directory includes are needed.

Wire format: `IG1|TYPE|requestId|arg1|arg2...\n` (LF only, ASCII, 115200 8N1).
Maximum line: 192 bytes including LF, eight arguments, 39 bytes per argument.
The decoder drops malformed/oversize lines and partial frames delayed more than
250 ms through the next LF. `requestId` is an unsigned decimal 32-bit value.
There is no escaped token syntax. UART is the local wired trust boundary.

| Message | Arguments | Behavior |
| --- | --- | --- |
| `HELLO` | `ttgo`/`beetle`, firmware, partition, boot ID | Boot ID changes on every reboot; repeated same-boot hello is not a restart. |
| `PING` | None | Beetle replies HELLO, HEART and its last OTA result. |
| `MODE` | enabled (`0`/`1`), profile (`live`/`training`) | Idempotent scan command. Replies HEART with the same request ID. |
| `HEART` | uptime ms, scan enabled, OTA busy | Sent every second. Unsolicited messages use ID 0. |
| `LOC` | room, RSSI dBm, age ms, valid (`0`/`1`) | Sent on room/validity changes and repeated every 1 s. ID increments per boot. Invalid room is `unknown`. |
| `OTA` | `check` | Nonzero unique request ID required. No keys/credentials cross UART. |
| `OTA` | `version`, positive firmware integer | Install/skip exactly this archived Beetle version; downgrades allowed. |
| `OTA_RESULT` | status, firmware | Echoes OTA request ID; statuses below. |
| `LOG` | boot ID, uptime ms, event, code, numeric value | Optional bounded diagnostics. Message ID is the per-boot log sequence; details below. |

`accepted` and `flashing` mean work is pending. TTGO must wait for `skipped` or
`updated` before updating itself. `updated` is emitted only after a reboot into a
different firmware version. For a pinned request, both `updated` and `skipped`
must match the exact requested version; `updated` also needs a changed boot ID
and matching HELLO firmware. `failed`, `disabled`, and `busy` stop the sequence.
The latest result/request is stored in NVS and replayed in response to PING, so a
lost reboot frame does not falsely time out or trigger a second flash. TTGO must
use fresh request IDs across its own reboots and allow 180 s worker timeout plus
30 s WDT recovery. Two boards must initially receive protocol v1 over USB.

`IoTGloveLocation.h` provides the fixed-size tracker (24 devices, 96 samples)
and reset-input latch used by Beetle. The tracker follows updated_IoTglove room
scoring: 1.5 s sample window, device median/EMA, room top-two mean, 5 dB/1.2 s
switching, and a global 5 s observation-loss timeout. LOC age measures time since
the latest accepted HAS3 observation while retaining the stable room. Both the
tracker and reset latch are compiled by the host tests.

## Optional Beetle diagnostics

`IG1|LOG|sequence|bootId|uptimeMs|event|code|value\n` always has five arguments.
`IoTGloveDiagnostics.h` supplies the strict producer/consumer parser and the
actual fixed-size queue used by Beetle. Existing IG1 receivers ignore unknown
`LOG` frames; HELLO, HEART, location, and OTA result formats are unchanged.

| event | code | numeric value |
| --- | --- | --- |
| `boot` | `reason` | ESP-IDF `esp_reset_reason()` value |
| `reset` | `requested` | GPIO1, encoded as `1` |
| `ota` | `wifi_start`, `wifi_ok`, `wifi_fail`, `checking`, `flashing`, `updated`, `skipped`, `failed`, `nvs_fail`, `task_fail`, `disabled` | OTA request ID |

The boot snapshot always uses sequence 1 and uptime 0. It is queued at startup
and after each PING, allowing TTGO to recover a missing boot cause after its own
restart. The receiver must cache/deduplicate the boot snapshot separately by boot
ID, instead of printing every PING replay. Other events use sequence 2 onward;
they must match the current HELLO boot ID and advance the event sequence before
being shown. LOG frames do not establish peer liveness or OTA success.

Beetle queues at most eight typed records. Queue access is protected by a short
FreeRTOS critical section, with no I/O inside it. The main loop sends at most two
records per iteration; a full queue drops the newest diagnostic record. Worker
callbacks never write UART directly. Only fixed event/code enums and integers
can be sent: no free-form log text, SSID, password, HMAC key, or HTTP content is
forwarded. Diagnostic events are best-effort and may be lost during a reset;
the existing HELLO/OTA_RESULT protocol remains the completion evidence.

`IoTGloveOta.h` supplies strict command, archive URL and metadata parsing shared
by both boards and host tests. `IoTGloveOtaClient` uses the existing HMAC-SHA256
key/format to authenticate archived board/version/partition/image metadata and
stream its exact image into an inactive OTA slot. It never changes partitions.
The legacy fixed-release path continues to use the external SecureOTA library.
