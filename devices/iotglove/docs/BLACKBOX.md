# TTGO blackbox diagnostics

The separate `libraries/HAS1BlackBox` logger records a bounded recent history on
the existing `min_spiffs` data partition. TTGO production and training builds use
the `iotglove` device identifier; `device.boot` v[3] distinguishes training.
Beetle does not link or run the logger. Game HTTP, shared libraries, deployed OTA
protocol, partition table and restart budgets are unchanged.

## Configuration

`IOTGLOVE_BLACKBOX_ENABLED` defaults to `1`.
`IOTGLOVE_BLACKBOX_BROKER_URI` defaults to `""`: flash only, no MQTT client.
Set `IOTGLOVE_BLACKBOX_MQTT_USERNAME` and `IOTGLOVE_BLACKBOX_MQTT_PASSWORD` in local
`secrets.h` or compiler definitions for a configured broker. Do not put credentials
inside the URI or commit populated secrets. Consult the library and standalone
`tools/windows-blackbox-logger` READMEs for supported transport, broker ACLs and
collector configuration. `--blackbox-mode off|flash|mqtt` supports build checks;
a compile check does not configure a field broker or deploy a device.

An empty/unknown partition is handled by the library's documented initialization
rules. Never erase a used filesystem automatically to make logging work.

## Device records

Numeric enums refer to the matching firmware source; retain its source archive.
Each row lists v[0..3]; the fixed text field names the subtype. Normal summaries
are emitted once per 60 seconds, first observed failures at most once per 10
seconds, game transitions at most once per second. Wi-Fi connection attempts and
link events are recorded as they occur. Producers enqueue fixed records only.

| Text subtype | Fields |
|---|---|
| `device.boot` | reset reason, firmware, partition version, training flag |
| `game` | phase, role, device state, reserved; `-1` means invalid/unknown |
| `primary.result/failure` | HAS2 HTTP error, HTTP status, elapsed ms, decoded snapshot valid |
| `primary.counts` | primary polls, invalid snapshots, maximum HTTP ms, reserved |
| `network.begin` | worker phase, reserved fields; sampled start, not every request |
| `network.summary` | phase, operation age ms, progress age ms, operation sequence |
| `main.summary` | maximum loop gap ms, loop count, reserved fields |
| `memory.main` | free heap, minimum heap, largest free block, main stack high water |
| `wifi` | connected, RSSI (`INT32_MIN` unavailable), connection failures, last loss reason |
| `recovery.worker_stack` | transport failures, forced reconnects, cooldown ms, worker stack high water |
| `output.adc_mv.batt_mv` | motor, lit pixels, ADC millivolts, calibrated battery millivolts |
| `beetle.online` | peer online flag, reserved fields |
| `link.lost` | disconnect reason, last known RSSI, reserved fields |
| `wifi.event` | Arduino event ID (association, GOT_IP, LOST_IP), reserved fields |
| `connect.begin/end` | failure count/start or connected result/end, failure count/end, reserved fields |
| `loss.context` | last valid phase, role, motor, battery millivolts |
| `loss.output_adc_heap` | motor age ms, lit pixels, raw ADC millivolts, heap |
| `restart.request` | cause (1 server, 2 worker stall, 3 link outage), restart attempts, reserved |
| `prior.link_loss` | retained RTC cause, reason, RSSI, restart attempt |
| `prior.loss_timing` | prior loss/noticed/restart uptime ms, connection failures |
| `prior.loss_context` | prior phase, role, motor, raw ADC millivolts |

ADC/battery fields use `-1` for unavailable/unmeasured; RSSI uses `INT32_MIN`.
These are device field contracts, independent of any collector-wide sentinel.
Live ADC readings preserve measured 0 mV when a sample is known. The legacy RTC
loss record has no validity flag, so retained zero can mean unread or actual zero;
blackbox export conservatively maps that ambiguous value to `-1`. The RTC layout
and its existing diagnostic behavior are unchanged. Raw ADC is not calibrated
battery voltage. Motor age `0xffffffff` in retained context means never observed. Numeric
uint32 counters/timestamps appear as signed int32 fields: reinterpret as unsigned
when calculating wrap-safe durations. A begin without a corresponding summary is
a lead, not proof of which function caused the failure.

## Interference boundaries and limits

Upload is permitted only with fresh known non-active/non-photo game state,
Wi-Fi and an idle/backoff worker. A game operation withdraws permission; it does
not wait for the uploader. MQTT has its own owner, finite memory/backoff and an
application ACK after SQLite commit. MQTT PUBACK alone never marks data as saved.
Independently of successful delivery, a full ring overwrites the oldest unsaved
sector and records a LOST range when possible; this is not lossless storage.
The radio, CPU and flash cache remain shared; no zero-latency promise is made.

LED rendering uses a zero-wait guard against logger flash work. A denied frame is
retried from the current desired output, and the motor-refresh deadline advances
only after an actual show. Motor GPIO updates are not gated. OTA sets a logger
pause flag and retains the existing OTA calls and watchdog exemption. Already
running flash operations cannot be preempted by that flag.

Restart deadlines and budgets are not extended. The final restart record is
best-effort RAM enqueue with **no wait** because the existing recovery deadline
has already expired; it can be lost. Previously stored records survive according
to the library's flash guarantees. RTC retained loss evidence is copied on warm
boot but is not reliable after power removal.

Typical steady operation generates approximately 10–15 device records per minute
before logger metadata; 1,920 physical slots therefore represent roughly 2–3 hours,
not weeks. This is a rate estimate, not a measured retention guarantee. Failures,
state changes, metadata and slot loss reduce that interval. The ring is a rolling
recent window and does not pin the first fault indefinitely. Successful uploads
retain the longer history on Windows. Sudden power collapse can lose RAM records;
software ADC sampling cannot prove a microsecond supply sag. Offline flash dump
and external voltage measurements remain useful when a device cannot reconnect.

## Validation status

Host policy tests cover unsigned wrap, bounded summary rate and fail-closed upload
conditions. `pixel_refresh_test.cpp` covers denied flash access preserving motor
refresh and post-motor resend requests. OTA dispatch tests assert pause during both
legacy OTA paths and resume on normal/exception return. Firmware compilation is
reported in the implementation verification report, not inferred from host tests.

Pending physical acceptance: logger off / flash only / MQTT, 30 minutes × 3 each;
Wi-Fi loss, broker/collector restart, hard power cut, LED/motor transitions, OTA
and 8-hour soak. Measure p99/max game-poll delay, loop gap, flash erase, heap and
stack high water. Keep telnet sessions at zero. Do not expand deployment before
these gates pass independently on TTGO and generator.

## Evidence schema 2 (20261024.2)

Validated Beetle LOG frames are preserved only after the existing parser and
boot/sequence tracker accept them. `beetle.log.values` stores peer boot ID, peer
uptime ms, peer sequence and value; `beetle.log.kind` stores matching boot/sequence
and the existing diagnostic event/code enums. These records' own boot/uptime are
TTGO **reception** context. The embedded peer uptime is not TTGO time or UTC.
`beetle.hello.state` records peer boot, firmware, partition and known/online flags
(bits 1/2). `beetle.reset.pulse` records TTGO-restart request, pulse length, known
baseline and peer boot. `beetle.reset.observed` records observation enum, baseline
boot and observed boot: a reset request is never treated as a confirmed reset cause.

All existing HTTP completion hooks now capture failure/error change/recovery by
request kind; a side-lane success does not reset a different kind's failure gate.
Types are 0 unknown, 1 own snapshot, 2 peer read, 3 send, 4 chip report, 5 management.
`http.failure`/`http.recovered`: operation ID, error, HTTP status, elapsed ms.
`http.phase.bytes.errno`: same ID, last result phase, body bytes, unavailable errno
(`INT32_MIN`). `http.ok_age.retry.kind`: same ID, last success age, Retry-After ms,
kind. The unchanged glove HTTP API does not expose socket errno or queue duration;
these are not inferred from an unrelated `errno`. A semantic invalid own snapshot
is separately `primary.failure`, with `primary.recovered` on observed validity.
Changes inside one second are coalesced and the latest pending result is emitted
on the next eligible completion; repeated identical failures summarize at 60 s.
This does not promise every intermediate transition or immediate recovery logging
when no later operation occurs.

`ap:<12 hex BSSID>` records event ID, reason, channel (`INT32_MIN` when not provided),
reserved. `wifi.ip.gw.mask` records IPv4, gateway, netmask canonical big-endian numeric uint32 values and IP-changed flag. Event payloads are copied directly, with no new scans.
`worker.stall` gives phase/operation/operation-age/progress-age; `wifi.down.threshold`
gives down-age/failures/was-up/reason. Existing restart budgets and OTA exemptions
are unchanged; `http.force_reconnect` records the actual policy decision.

`adc.min.age.n.batt_age` gives minimum **ADC pin mV** in the summary window, age of
last raw ADC sample, sample count and age of the last valid calibrated battery
estimate. It reuses only ADC reads already performed by the application. No sample
means `INT32_MIN`, not zero. `output.applied` records actual applied motor state,
lit pixels, brightness8 and number of coalesced output changes; edges are limited
to one record per 500 ms and periodic output summaries still record final state.
These slow samples cannot prove or rule out a fast supply droop. No voltage sensor,
ADC calibration or additional blocking ADC I/O was introduced.
