"""Bounded, optional Omada UDP Syslog ingestion; independent of device MQTT ACKs.

UDP is lossy and source filtering is not authentication. All sender/site bindings
are operator configured. Timestamp quality is evidence, never a clock-sync claim.
"""
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
import ipaddress
import json
import queue
import re
import socket
import threading

UTC = timezone.utc
MAC_PATTERN = r"(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}"
MONTHS = {name: number for number, name in enumerate(
    ("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"), 1)}
IPV4_PATTERN = r"(?:\d{1,3}\.){3}\d{1,3}"
# AP traffic logging observed from an EAP: "[<epoch>.<ns>] AP MAC=.. MAC SRC=.. IP SRC=.. ..."
TRAFFIC_RECORD = re.compile(
    r"\[(\d{9,11})\.(\d{1,9})\]\s+AP MAC=(" + MAC_PATTERN + r")\s+MAC SRC=(" + MAC_PATTERN + r")\s+"
    r"IP SRC=(" + IPV4_PATTERN + r")\s+IP DST=(" + IPV4_PATTERN + r")\s+IP proto=(\d{1,3})(?=\s|$)"
    r"(?:\s+SPT=(\d{1,5})(?=\s|$))?(?:\s+DPT=(\d{1,5})(?=\s|$))?")
# Records stay only in the raw datagram; fields keep a bounded summary.
MAX_TRAFFIC_SUMMARY_ITEMS = 32
# EAP epochs outside this window of arrival are treated as bogus, not as source time.
MAX_TRAFFIC_EPOCH_SKEW = timedelta(days=1)
MAX_LIST_ITEMS = 8
MAX_LIST_TEXT = 128
# telemetry.Event allows 16 KiB of fields; leave room for adapter-added keys.
MAX_DETAILS_BYTES = 12288


@dataclass(frozen=True)
class OmadaEvent:
    source: str
    site_id: str
    source_ip: str
    observed_utc: str
    event_utc: str | None
    time_quality: str
    event_type: str
    device_mac: str | None
    ap_mac: str | None
    details: dict
    raw: bytes


def _mac(value):
    return value.replace(":", "").replace("-", "").upper()


def _identities(message, label):
    # Only explicitly labelled identities: never assume the first MAC is a client.
    bracket = re.findall(r"\[" + label + r":[^\]\r\n]*?(" + MAC_PATTERN + r")\]", message, re.I)
    plain = re.findall(r"\b" + label + r"(?:\s+MAC)?\s*[:=]?\s+(" + MAC_PATTERN + r")(?![0-9A-Fa-f:-])", message, re.I)
    return list(dict.fromkeys(_mac(value) for value in bracket + plain))


def _names(message, label):
    # Omada event text names devices as "[client:<name>:<MAC>]"; keep the name only when present.
    found = re.findall(r"\[" + label + r":([^\]\r\n]{1,128}?):" + MAC_PATTERN + r"\]", message, re.I)
    return _bounded(found)


def _bounded(values):
    return list(dict.fromkeys(value[:MAX_LIST_TEXT] for value in values))[:MAX_LIST_ITEMS]


def _traffic_records(message):
    """Return complete records and the number of records cut off at the datagram end."""
    records = []
    partial = 0
    for match in TRAFFIC_RECORD.finditer(message):
        if match.end() == len(message):
            # The sender cuts datagrams mid-record; a value at the very end may be incomplete.
            partial += 1
            continue
        seconds, fraction, ap, src_mac, src_ip, dst_ip, proto, spt, dpt = match.groups()
        try:
            stamp = datetime.fromtimestamp(int(seconds), UTC) + timedelta(microseconds=int(fraction.ljust(9, "0")[:6]))
        except (OverflowError, OSError, ValueError):
            stamp = None
        records.append({"utc": stamp.isoformat() if stamp else None, "ap_mac": _mac(ap),
                        "src_mac": _mac(src_mac), "src_ip": src_ip, "dst_ip": dst_ip,
                        "proto": int(proto), "spt": int(spt) if spt else None,
                        "dpt": int(dpt) if dpt else None})
    return records, partial


def parse_syslog(payload, source_ip, observed_at, site_id, utc_offset_minutes=540,
                 max_datagram=8192):
    """Preserve bounded raw input, including unknown/malformed formats.

    RFC3164 has neither year nor UTC offset: nearest local year is an estimate.
    Offset-aware RFC5424 identifies source time but does not prove NTP accuracy.
    """
    if observed_at.tzinfo is None or observed_at.utcoffset() is None:
        raise ValueError("observed_at must be timezone aware")
    if not isinstance(payload, bytes) or not 256 <= max_datagram <= 8192:
        raise ValueError("payload must be bytes and limit must be 256..8192")
    if not isinstance(site_id, str) or not site_id.strip() or len(site_id) > 128:
        raise ValueError("explicit site_id required (1..128 characters)")
    if isinstance(utc_offset_minutes, bool) or not isinstance(utc_offset_minutes, int) or not -1439 <= utc_offset_minutes <= 1439:
        raise ValueError("invalid UTC offset")
    source_ip = str(ipaddress.IPv4Address(source_ip))
    observed_at = observed_at.astimezone(UTC)
    raw = payload[:max_datagram]
    text = raw.decode("utf-8", errors="replace")
    details = {"truncated": len(payload) > max_datagram,
               "received_bytes": len(payload), "timestamp_flags": [],
               "site_binding": "operator_configured_sender_destination"}
    event_at = None
    quality = "missing_or_invalid"
    local_zone = timezone(timedelta(minutes=utc_offset_minutes))
    body = re.sub(r"^<\d{1,3}>", "", text)
    traffic, partial_traffic = _traffic_records(text)
    # RFC5424 VERSION TIMESTAMP HOSTNAME APP-NAME PROCID MSGID STRUCTURED-DATA.
    # Omada controllers send "YYYY-MM-DD HH:MM:SS" (space, no offset) in site local time.
    modern = re.match(r"^\d{1,3}\s+(?:(\d{4}-\d{2}-\d{2})[T ](\d{2}:\d{2}:\d{2}(?:\.\d{1,6})?)(Z|[+-]\d{2}:\d{2})?(?=\s)|(\S+))", body)
    if modern:
        date, clock, zone, other = modern.groups()
        if date:
            try:
                if zone:
                    event_at = datetime.fromisoformat(f"{date}T{clock}{zone.replace('Z', '+00:00')}").astimezone(UTC)
                    quality = "source_offset_unverified"
                else:
                    event_at = datetime.fromisoformat(f"{date}T{clock}").replace(tzinfo=local_zone).astimezone(UTC)
                    quality = "naive_local_configured_offset"
                    details["assumed_utc_offset_minutes"] = utc_offset_minutes
                    details["timestamp_flags"].append("naive_source_timestamp_configured_offset")
            except ValueError:
                details["timestamp_flags"].append("invalid_or_naive_source_timestamp")
        elif other != "-":
            details["timestamp_flags"].append("invalid_or_naive_source_timestamp")
    else:
        legacy = re.match(r"^([A-Z][a-z]{2})\s+(\d{1,2})\s+(\d{2}):(\d{2}):(\d{2})(?:\s|$)", body)
        if legacy:
            month, day, hour, minute, second = legacy.groups()
            if month in MONTHS:
                local = observed_at.astimezone(local_zone)
                # Some senders repeat the full local date in the message; use its year only
                # when month/day/time agree with the header.
                years = [int(year) for year, m, d, hh, mm, ss in re.findall(
                    r"\b(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})\b", body[legacy.end():])
                    if (int(m), int(d), hh, mm, ss) == (MONTHS[month], int(day), hour, minute, second)]
                candidates = []
                for year in years[:1] or (local.year - 1, local.year, local.year + 1):
                    try:
                        candidates.append(datetime(year, MONTHS[month], int(day), int(hour), int(minute), int(second), tzinfo=local_zone).astimezone(UTC))
                    except ValueError:
                        pass
                if candidates:
                    event_at = min(candidates, key=lambda candidate: abs((candidate - observed_at).total_seconds()))
                    details["assumed_utc_offset_minutes"] = utc_offset_minutes
                    if years:
                        quality = "naive_local_configured_offset"
                        details["timestamp_flags"].append("naive_source_timestamp_configured_offset")
                    else:
                        quality = "estimated_year_configured_offset"
                        details["timestamp_flags"].append("year_ambiguous")
    # Per-record epoch seconds are absolute UTC and finer than the header; prefer the first one.
    first_traffic = next((record["utc"] for record in traffic if record["utc"]), None)
    if first_traffic:
        traffic_at = datetime.fromisoformat(first_traffic)
        if abs(traffic_at - observed_at) <= MAX_TRAFFIC_EPOCH_SKEW:
            event_at = traffic_at
            quality = "source_epoch_unverified"
            details.pop("assumed_utc_offset_minutes", None)
            details["timestamp_flags"] = [flag for flag in details["timestamp_flags"] if flag != "year_ambiguous"]
        else:
            details["timestamp_flags"].append("traffic_epoch_implausible")
    if event_at is not None:
        delta = (observed_at - event_at).total_seconds()
        details["observed_minus_event_seconds"] = delta
        if delta < -5:
            details["timestamp_flags"].append("source_time_in_future")
        if delta > 300:
            details["timestamp_flags"].append("delayed_or_clock_skewed")
    declared_sites = list(dict.fromkeys(re.findall(r"\[site:([^\]\r\n]{1,128})\]", text, re.I)))
    details["declared_site_names"] = declared_sites
    if declared_sites:
        details["site_binding"] = "configured_sender_payload_site_unverified"
    # Extract only explicitly labelled values. These names are parser contracts,
    # not a claim that every Omada version emits them. Conflicts remain arrays.
    explicit = {}
    for label in ("rssi", "reason", "channel", "client_ip", "ap_ip", "dhcp_type"):
        matches = re.findall(r"\["+label+r"\s*[:=]\s*([^\]\r\n]{1,128})\]", text, re.I)
        if matches:
            explicit[label] = _bounded(matches)
    # Omada event sentences, e.g. '[client:..] (IP: 1.2.3.4) went online on [ap:..] with SSID "x" on channel 6.'
    for label, pattern in (("client_ip", r"\(IP:\s*(" + IPV4_PATTERN + r")\)"),
                           ("ssid", r'\bSSID "([^"\r\n]{1,64})"'),
                           ("channel", r"\bon channel (\d{1,3})\b"),
                           ("allocated_ip", r"\ballocated IP address (" + IPV4_PATTERN + r")\b")):
        matches = re.findall(pattern, text, re.I)
        if matches:
            explicit[label] = _bounded(explicit.get(label, []) + matches)
    details["explicit_fields"] = explicit
    clients = _identities(text, "client")
    aps = _identities(text, "ap")
    if partial_traffic:
        details["traffic_partial_records"] = partial_traffic
    if traffic:
        # "AP MAC=" is an explicit label; MAC SRC may be either end of the flow, so not a client.
        aps = list(dict.fromkeys(aps + [record["ap_mac"] for record in traffic]))
        stamps = [record["utc"] for record in traffic if record["utc"]]
        details["traffic_record_count"] = len(traffic)
        details["traffic_first_utc"] = min(stamps) if stamps else None
        details["traffic_last_utc"] = max(stamps) if stamps else None
        truncated = False
        for key, values in (("traffic_src_macs", [record["src_mac"] for record in traffic]),
                            ("traffic_src_ports", [record["spt"] for record in traffic if record["spt"] is not None]),
                            ("traffic_dst_ports", [record["dpt"] for record in traffic if record["dpt"] is not None])):
            distinct = list(dict.fromkeys(values))
            truncated = truncated or len(distinct) > MAX_TRAFFIC_SUMMARY_ITEMS
            details[key] = distinct[:MAX_TRAFFIC_SUMMARY_ITEMS]
        details["traffic_summary_truncated"] = truncated
    details["client_macs"] = clients
    details["ap_macs"] = aps
    for key, label in (("client_names", "client"), ("ap_names", "ap")):
        names = _names(text, label)
        if names:
            details[key] = names
    if len(clients) > 1:
        details["ambiguous_client"] = True
    event_type = "traffic_log" if traffic else "unknown"
    for name, pattern in (("dhcp_lease", r"\bDHCP\b[^.\r\n]{0,40}\ballocated IP address\b"),
                          ("roaming", r"\broam(?:ed|ing|s)?\b"),
                          ("disconnected", r"\bdisconnect(?:ed|ing|s)?\b|\bdisassociat(?:ed|ion)\b"
                           + (r"|\bwent offline\b" if clients else "")),
                          ("connection_failed", r"\bfailed to connect\b|\bconnection failed\b"),
                          ("connected", r"\bconnected\b|\bassociated\b" + (r"|\bwent online\b" if clients else "")),
                          ("device_online", r"\bwent online\b"),
                          ("device_offline", r"\boffline\b"),
                          ("device_restart", r"\breboot(?:ed|ing)?\b|\brestart(?:ed|ing)?\b")):
        if event_type != "unknown":
            break
        if re.search(pattern, text, re.I):
            event_type = name
    details["classification"] = "heuristic" if event_type != "unknown" else "unknown"
    # Invalid bytes become U+FFFD (3 bytes in JSON); drop derived extras rather than fail storage.
    # The raw datagram is always retained, so dropped fields can be re-derived later.
    for key in ("client_names", "ap_names", "explicit_fields", "traffic_src_ports", "traffic_src_macs"):
        if len(json.dumps(details, ensure_ascii=False).encode("utf-8")) <= MAX_DETAILS_BYTES:
            break
        if key in details:
            details.pop(key)
            details.setdefault("fields_dropped_for_size", []).append(key)
    return OmadaEvent("omada", site_id, source_ip, observed_at.isoformat(),
                      event_at.isoformat() if event_at else None, quality, event_type,
                      clients[0] if len(clients) == 1 else None,
                      aps[0] if len(aps) == 1 else None, details, raw)


def to_telemetry(event):
    """Adapt one received datagram without inventing vendor identity or accuracy."""
    import base64
    import uuid
    from telemetry import Event
    fields = dict(event.details)
    fields["ap_mac"] = event.ap_mac
    fields["raw_encoding"] = "base64"
    return Event(site_id=event.site_id, source="omada", source_id=event.source_ip,
                 # Syslog supplies no reliable per-message identity. Repeated identical
                 # datagrams may represent real events: retain each arrival separately.
                 event_id=uuid.uuid4().hex, observed_utc=event.observed_utc,
                 occurred_utc=event.event_utc, clock_quality=event.time_quality,
                 uncertainty_ms=None, mac=event.device_mac, event_type=event.event_type,
                 fields=fields, raw=base64.b64encode(event.raw).decode("ascii"))


class OmadaReceiver:
    """Two bounded threads; callback runs only on consumer thread.

    The callback should persist into a separate DB, with bounded DB timeouts.
    stop() returns False if callback remains blocked; callers must report this.
    An optional on_close runs on the consumer thread for SQLite owner cleanup.
    """
    def __init__(self, bind_host, port, source_sites, on_event, queue_capacity=256,
                 max_datagram=8192, utc_offset_minutes=540, on_close=None, expected_site_name=None):
        self.bind_host = str(ipaddress.IPv4Address(bind_host))
        if not isinstance(port, int) or isinstance(port, bool) or not 0 <= port <= 65535:
            raise ValueError("invalid UDP port")
        if not source_sites or not isinstance(source_sites, dict):
            raise ValueError("explicit source-IP to site bindings required")
        self.source_sites = {}
        for source, site in source_sites.items():
            key = str(ipaddress.IPv4Address(source))
            if not isinstance(site, str) or not site.strip() or len(site) > 128:
                raise ValueError("explicit site identifier required")
            self.source_sites[key] = site
        if not isinstance(queue_capacity, int) or isinstance(queue_capacity, bool) or not 1 <= queue_capacity <= 4096:
            raise ValueError("queue capacity must be 1..4096")
        if not isinstance(max_datagram, int) or isinstance(max_datagram, bool) or not 256 <= max_datagram <= 8192:
            raise ValueError("datagram limit must be 256..8192")
        if not isinstance(utc_offset_minutes, int) or isinstance(utc_offset_minutes, bool) or not -1439 <= utc_offset_minutes <= 1439:
            raise ValueError("invalid UTC offset")
        if expected_site_name is not None and (not isinstance(expected_site_name, str) or not expected_site_name.strip() or len(expected_site_name) > 128):
            raise ValueError("expected site name must be bounded nonempty text")
        self.expected_site_name = expected_site_name
        self.port = port
        self.on_event = on_event
        self.on_close = on_close
        self.max_datagram = max_datagram
        self.utc_offset_minutes = utc_offset_minutes
        self.queue = queue.Queue(maxsize=queue_capacity)
        self._stop = threading.Event()
        self._producer_done = threading.Event()
        self._lock = threading.Lock()
        self._counts = {name: 0 for name in ("received", "filtered", "site_mismatch", "queued", "overflow", "truncated", "stored", "callback_errors", "socket_errors", "close_errors")}
        self._socket = None
        self._threads = []
        self._started = False

    def _count(self, name):
        with self._lock:
            self._counts[name] += 1

    def stats(self):
        with self._lock:
            result = dict(self._counts)
        result["queue_size"] = self.queue.qsize()
        return result

    def start(self):
        if self._started:
            raise RuntimeError("receiver instances may only start once")
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            # Deliberately no SO_REUSEADDR: a second collector must not steal logs.
            sock.bind((self.bind_host, self.port))
            sock.settimeout(0.2)
        except Exception:
            sock.close()
            raise
        self._socket = sock
        self.port = sock.getsockname()[1]
        self._started = True
        self._threads = [threading.Thread(target=self._receive, name="omada-udp", daemon=True),
                         threading.Thread(target=self._consume, name="omada-store", daemon=True)]
        for thread in self._threads:
            thread.start()

    def submit(self, payload, source_ip, observed_at=None):
        """Nonblocking ingress also used by deterministic tests/importers."""
        if self._stop.is_set():
            return False
        self._count("received")
        site = self.source_sites.get(source_ip)
        if site is None:
            self._count("filtered")
            return False
        event = parse_syslog(payload, source_ip, observed_at or datetime.now(UTC), site,
                             self.utc_offset_minutes, self.max_datagram)
        if self.expected_site_name is not None:
            declared = event.details["declared_site_names"]
            if any(name != self.expected_site_name for name in declared):
                self._count("site_mismatch")
                return False
            event.details["expected_site_name"] = self.expected_site_name
            if declared:
                event.details["site_binding"] = "configured_sender_matching_payload_site"
        if event.details["truncated"]:
            self._count("truncated")
        try:
            self.queue.put_nowait(event)
        except queue.Full:
            self._count("overflow")
            return False
        self._count("queued")
        return True

    def _receive(self):
        try:
            while not self._stop.is_set():
                try:
                    payload, address = self._socket.recvfrom(65535)
                    self.submit(payload, address[0])
                except socket.timeout:
                    continue
                except OSError:
                    if not self._stop.is_set():
                        self._count("socket_errors")
                    break
        finally:
            self._producer_done.set()

    def _consume(self):
        try:
            while not self._producer_done.is_set() or not self.queue.empty():
                try:
                    event = self.queue.get(timeout=0.1)
                except queue.Empty:
                    continue
                try:
                    self.on_event(event)
                    self._count("stored")
                except Exception:
                    # UDP has no ACK/replay. Count storage losses without stopping MQTT.
                    self._count("callback_errors")
                finally:
                    self.queue.task_done()
        finally:
            if self.on_close:
                try:
                    self.on_close()
                except Exception:
                    self._count("close_errors")

    def is_receiving(self):
        return bool(self._threads and self._threads[0].is_alive())

    def stop(self, timeout=5.0):
        import time
        if timeout < 0:
            raise ValueError("timeout must be nonnegative")
        self._stop.set()
        if self._socket:
            self._socket.close()
        if not self._started:
            self._producer_done.set()
        deadline = time.monotonic() + timeout
        for thread in self._threads:
            thread.join(max(0, deadline - time.monotonic()))
        return all(not thread.is_alive() for thread in self._threads)
