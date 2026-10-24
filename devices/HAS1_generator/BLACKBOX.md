# Generator blackbox diagnostics

`libraries/HAS1BlackBox` is a separate optional logger. It records a rolling recent
window in the existing `min_spiffs` partition and uploads through MQTT to the
independent Windows collector in `tools/windows-blackbox-logger`. No game request
is converted to MQTT. Existing bounded network worker, shared libraries, RTC crash
journal, watchdog policy, OTA calls and partition layout remain unchanged.

This implementation targets the actual current generator source: it already has a
single network worker and preserves NVS. The older plan's unconditional NVS erase
and unobservable `SituationAsync` assumptions no longer describe this baseline.
Contribution messages now use the existing observable network queue. This change
does not introduce an NVS erase or alter that network architecture.

## Configuration

`HAS1_GENERATOR_BLACKBOX_ENABLED` defaults to `1`.
`HAS1_GENERATOR_BLACKBOX_BROKER_URI` defaults to `""` (flash-only).
`HAS1_GENERATOR_BLACKBOX_MQTT_USERNAME` and
`HAS1_GENERATOR_BLACKBOX_MQTT_PASSWORD` default to empty; configure local
`secrets.h` or compiler definitions for an authenticated broker. Follow the library
and collector READMEs for the supported broker transport and ACLs. Do not commit
credentials. Set enabled to `0` for the logger-off comparison. Off avoids logger
storage/network tasks; small device observation hooks remain in the binary.

Boot starts the logger before potentially blocking peripheral initialization.
Initialization errors disable only logging; unknown partition contents are not
automatically formatted. An empty SPIFFS left by an older `min_spiffs` layout
(format markers only, no file) is erased once and initialized, per the library README.
Generators flashed under Arduino 2.x keep a 192 KiB `spiffs` partition (OTA never
rewrites the table); the ring uses its first 128 KiB and checks the whole partition.

Each Telnet session's first `[diagnostics]` line ends with the logger state:
`bb_ready` (0/1), `bb_status` (library `RingStatus`: 0 unopened, 1 ready, 2 bad
size, 3 read error, 4 metadata conflict, 5 foreign data left untouched, 6 erase
error, 7 write error, 8 record conflict), `bb_partition` (spiffs bytes, 0 = none),
`bb_flash_errors`, `bb_uploaded`, `bb_upload_failures` and `bb_dropped`. A logger
that never connects to the broker can be diagnosed from this line without USB. Up to eight valid prior RTC events are queued as paired event/time records before
RTC initialization: earliest retained negative-code fault plus the newest seven
when that fault is outside the tail, otherwise the newest eight. This copy is best effort and
not evidence preserved through power removal.

## Records

Normal summaries and matched operation samples are emitted at most once per
60 seconds; legacy operation fault samples have a separate 10-second gate.
Schema 2 HTTP change details use the one-second coalescing described below.
Wi-Fi events remain transition-driven. Fields below are v[0..3].

| Text subtype | Fields |
|---|---|
| `device.boot` | reset reason, firmware, partition version, reserved |
| `main.valid_age` | max loop gap ms, loop count, last valid ReceiveMine age ms (`-1` unknown), reserved |
| `memory.main` | free heap, min heap, largest free block, main stack high water |
| `wifi` | Wi-Fi status, RSSI (`INT32_MIN` unavailable), reserved fields |
| `encoder.wire.tagger.led` | encoder value, displayed wire count, tagger active, brightness |
| `adc.battery.unavailable` | `-1`, `-1`, reserved; generator has no calibrated voltage hook |
| `network.summary` | active request ID, transport phase, request age ms, worker progress age ms |
| `worker.stack.busy.ota` | worker stack high water, queue busy, maintenance, reserved |
| `worker.result/failure` | request kind, transport error, HTTP status, elapsed ms |
| `worker.counts` | total completions, failed/stale/non-2xx completions, max ms, latest stale flag |
| `snapshot.valid` | ReceiveMine decode/application valid, last valid ReceiveMine age ms, reserved |
| phase/state text | v[0] = 0 phase / 1 device state, remaining fields reserved |
| `poll.invalid` | failed transport/management snapshot; fields reserved |
| `wifi.event` | Arduino event ID, disconnect reason where applicable, reserved |
| `operation.sample` | diagnostic operation ID, `(event << 8) | phase`, result code, elapsed ms |
| `server.restart` | server restart cause = 1, reserved fields |
| `prior.rtc.event` | prior sequence, `(event << 8) | phase`, result code, detail |
| `prior.rtc.time` | matching prior sequence, prior uptime ms, elapsed ms, operation ID |

RTC event/phase IDs are defined in `generator_diagnostics.h`; network enums are in
`generator_network.h` and the unchanged bounded-transport header. Keep matching
firmware source archives. Signed fields containing unsigned counters/timestamps
must be interpreted as uint32 for rollover arithmetic. A sampled BEGIN without
END is a lead: queue loss, power cut and sampling also affect what is retained.
HTTP success and valid application snapshot are separate records.

## Non-interference policy

Uploading requires known non-active/non-photo state and a recent valid server
response. A successful management poll refreshes server reachability while
`main.valid_age` still reports the age of the actual ReceiveMine snapshot. No
upload starts while the network queue/mailbox, Wi-Fi scan, driver fault or OTA
maintenance is busy. Queuing game work withdraws upload permission immediately;
the game never waits for MQTT. An in-flight upload may take time to stop, and all
traffic shares Wi-Fi airtime.

`NeoShow` acquires the logger flash guard with zero wait. Contention preserves a
pending bit per strip; the next loop sends the latest existing pixel buffer.
Transport errors retain the existing RMT quarantine behavior and do not become
automatic retries. Diagnostic fault hooks run outside the RTC lock. Logger tasks
never feed the main or worker watchdog.

OTA adds pause/unpause around the existing calls; it cannot preempt flash work
already executing. Explicit server restart stays immediate; the final log is
best-effort queueing, with no new flush delay or restart policy.

Steady state is approximately 10–15 records/minute, plus logger metadata, so 1,920
physical slots hold roughly 2–3 hours in ordinary operation. This is an estimate;
operation/fault activity can shorten it considerably. The rolling window does
not preserve the first fault indefinitely or reconstruct past weeks. SQLite holds
the longer history after successful upload. MQTT transport PUBACK is not the
collector's durable-save ACK. Log loss and sensor limitations must be considered
before diagnosing power, Wi-Fi or firmware as a proven cause.

## Verification

`tests/run_blackbox_tests.py` compiles policy tests and the actual NeoShow wrapper
with ASan/UBSan: denied flash guard, latest-frame replacement, pending retry,
invalid strip and no automatic retry after RMT failure. Existing boot/network,
RTC diagnostic and remaining generator behavior suites run with inert logger
hooks, retaining assertions against calls under network/diagnostic locks.

These tests do not validate ESP32 flash/cache timing, Windows behavior or physical
power loss. Independently run off / flash / MQTT modes (30 min × 3), an 8-hour soak,
Wi-Fi and broker outages, hard power cuts, OTA, all four LED strips, encoder/PCNT,
RFID, DFPlayer and wire transitions before field rollout. Keep telnet clients at
zero. Record poll/loop p99/max delays, erase timing, heap, stack and queue loss.

## Evidence schema 2 (20261024.2)

The separate library adds a schema/build marker and eight-slot best-effort warm
RTC replay; see its README for power-loss and duplicate limits. Generator's existing
32-event RTC snapshot now exports the earliest retained negative-code fault plus
the newest seven (or newest eight when the fault is already in that tail).
`prior.rtc.coverage` reports total prior events, invalid count, omitted count, warm
flag. It does not extend the old RTC window or claim preservation for weeks.
`operation.detail` retains detail previously discarded by MQTT: ID,event,phase,
detail. It uses the existing bounded operation/fault selection.

Failure/change/recovery details are selected separately for each NetworkKind, with
one-second coalescing and 60-second repeated-failure summaries. No payload or query
is logged. HTTP 200 is still not a proof of successful application interpretation.

| Text | Four values |
|---|---|
| `http.failure.kind` / `http.recovered.kind` | request ID, kind, transport error, HTTP status |
| `http.phase.errno.body_bytes` | request ID, result phase, socket errno, retained body bytes (not all wire bytes) |
| `http.queue.total.discard` | request ID, queue ms, total ms, discard enum |
| `http.connect.write.first` | request ID, connect ms, write ms, first-byte ms |
| `http.header.body.close` | request ID, header ms, body ms, close ms |
| `http.ok_age.stale.generation` | request ID, previous success age ms (`INT32_MIN` unknown), stale flag, generation |
| `worker.queue.gates` | queue depth, bitmask: 1 mailbox / 2 scan pending / 4 driver fault / 8 maintenance, Wi-Fi status, reserved |

Wi-Fi event-only BSSID/channel/IP/gateway/netmask snapshots use the same schema as
the glove. The callback copies event data and does no scan or blocking driver query.
Queue/gate changes coalesce to one record per second. The generator still has no
voltage measurement; unavailable telemetry is explicit, not synthesized.

Timing fields preserve the existing transport result. A zero for a phase that was
never reached is an unmeasured/default value, not proof of zero-latency I/O. Error
and failure phase must accompany timing interpretation.
