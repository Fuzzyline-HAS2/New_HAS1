# Omada Syslog ingestion and time evidence

The optional UDP listener receives Omada Remote Logging without controller login,
API polling, firmware changes, or changes to the device MQTT durable-ACK path.
The listener has its own bounded queue and consumer; telemetry is stored separately
from the device record database. Collector settings use the optional `telemetry` section; see `config.store1.example.json`.

## Site 1 deployment prerequisites

1. Verify the real Omada **site name and site identity**. `172.30.1.1` is only the
   sender/controller address: one controller may manage multiple sites.
2. Enable Remote Logging **only for the verified site 1** to the Windows collector's
   fixed LAN address, UDP port `5514`. Enable Client Detail Logs if the installed
   controller offers it. Another site's logs must not use this same source-IP/site
   binding and destination; assign a separate destination/receiver instead.
3. Configure an explicit sender-IP → internal site-ID binding, such as
   `172.30.1.1` → `store-1`, only after step 2. This is an operator assertion, not
   automatic discovery or cryptographic proof. Some installations forward directly
   from APs; only add verified AP sender IPs, never allow every LAN address.
4. Allow inbound UDP 5514 in Windows Firewall only from the configured sender IPs
   on the trusted private network. This implementation does not change the firewall
   or controller automatically and does not expose the receiver to the Internet.
5. Synchronize Windows, controller and game server clocks using NTP/time services.
   Record observed clock offset and uncertainty when comparing events. Setting all
   displays to Korea time does not synchronize their clocks.
6. Trigger a controlled test-client disconnect/reconnect and confirm the received
   raw log, client MAC, AP, source timestamp and site. Real Omada firmware formats
   and log coverage vary. Synthetic parser tests do not prove this installation
   sends all roaming, disconnect reason or RSSI information.

Official references: [Omada Remote Logging and Client Detail Logs](https://www.tp-link.com/us/user-guides/omada-sdn-software-controller/chapter-4-configure-the-network-with-omada-sdn-controller.html),
[Omada log-server configuration](https://www.omadanetworks.com/us/document/13012/).
No controller credentials are needed or stored by this receiver.

## What is retained

- Raw datagram bytes, bounded to 8 KiB; telemetry adaptation stores base64 to retain
  invalid UTF-8. Oversized datagrams retain the prefix and explicit truncation/size.
- Source IP, explicitly configured site ID, Windows observation time in UTC.
- Source event UTC when parseable, with an explicit time-quality classification.
- Heuristic event classification (connection, disconnection, roaming, restart,
  offline or unknown). Classification is not a verified controller schema.
- Client/AP MACs only where the message explicitly labels them. Multiple client
  MACs leave the primary device MAC unset; roaming retains both AP MACs without
  guessing which is the device. AP names are still available in the raw log.
- Explicit `[site:...]` labels are preserved as `declared_site_names`, with
  `site_binding=configured_sender_payload_site_unverified`. They are not silently
  treated as proof of the configured internal site ID. With `expected_site_name` configured, any explicit contradictory site label is
  rejected before storage and counted as `site_mismatch`. A matching label still
  does not authenticate the sender. Logs without site labels rely on the dedicated
  destination configuration.

Unknown formats are retained rather than discarded. This initial parser does not
promise vendor-specific RSSI, channel, reason-code or DHCP/IP fields; the raw
payload permits later parser improvements after real samples are available.

## Time interpretation

| Input | Stored interpretation |
|---|---|
| RFC5424 explicit `Z` or UTC offset | Source UTC, `source_offset_unverified` |
| RFC3164 `Oct 24 12:00:00` | Nearest year and configured offset (default UTC+09:00), `estimated_year_configured_offset` |
| Missing, malformed or naive RFC5424 timestamp | No event UTC; observation UTC remains separate |

RFC3164 has no year or zone. Nearest-year selection is always marked ambiguous;
logs delayed more than roughly half a year cannot be dated reliably. A timestamp
more than 5 seconds ahead or 300 seconds behind arrival gets a diagnostic flag.
These flags do not distinguish network delay from clock error. Explicit UTC does
not prove the controller's clock is synchronized. Never label inferred time as
exact, and never substitute Windows arrival time for device occurrence time.

## Reliability boundaries

UDP can lose, duplicate, reorder and spoof packets. The allowlist filters sources
but does not authenticate them. There is no syslog durable ACK or replay protocol.
The queue is bounded (default 256, maximum 4096); overflow drops incoming records
and increments counters. A storage callback error counts a lost event and permits
subsequent events to proceed; it never blocks the glove MQTT ACK path. Monitor
`overflow`, `truncated`, `callback_errors`, `socket_errors` and `close_errors`.

Stopping closes the UDP socket and drains already queued events. If a storage
callback stays blocked beyond the shutdown deadline, stop returns failure; it
must not be described as a successful flush. The receiver uses no address reuse,
so duplicate listeners fail to bind instead of silently splitting packets.

CSV 보충은 `import_omada_csv.py`에서 실제 헤더를 명시적으로 매핑한다. 특정 Omada CSV
스키마를 가정하지 않는다. 조건·한계와 명령은 [EVIDENCE.md](EVIDENCE.md)에 있다.
