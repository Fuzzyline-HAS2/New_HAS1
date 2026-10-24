"""Bounded, optional Omada UDP Syslog ingestion; independent of device MQTT ACKs.

UDP is lossy and source filtering is not authentication. All sender/site bindings
are operator configured. Timestamp quality is evidence, never a clock-sync claim.
"""
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
import ipaddress
import queue
import re
import socket
import threading

UTC = timezone.utc
MAC_PATTERN = r"(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}"
MONTHS = {name: number for number, name in enumerate(
    ("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"), 1)}


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
    body = re.sub(r"^<\d{1,3}>", "", text)
    # RFC5424 VERSION TIMESTAMP HOSTNAME APP-NAME PROCID MSGID STRUCTURED-DATA.
    modern = re.match(r"^\d{1,3}\s+(\S+)\s+", body)
    if modern:
        stamp = modern.group(1)
        if re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,6})?(?:Z|[+-]\d{2}:\d{2})", stamp):
            try:
                event_at = datetime.fromisoformat(stamp.replace("Z", "+00:00")).astimezone(UTC)
                quality = "source_offset_unverified"
            except ValueError:
                pass
        elif stamp != "-":
            details["timestamp_flags"].append("invalid_or_naive_source_timestamp")
    else:
        legacy = re.match(r"^([A-Z][a-z]{2})\s+(\d{1,2})\s+(\d{2}):(\d{2}):(\d{2})(?:\s|$)", body)
        if legacy:
            month, day, hour, minute, second = legacy.groups()
            if month in MONTHS:
                local = observed_at.astimezone(timezone(timedelta(minutes=utc_offset_minutes)))
                candidates = []
                for year in (local.year - 1, local.year, local.year + 1):
                    try:
                        candidates.append(datetime(year, MONTHS[month], int(day), int(hour), int(minute), int(second), tzinfo=local.tzinfo).astimezone(UTC))
                    except ValueError:
                        pass
                if candidates:
                    event_at = min(candidates, key=lambda candidate: abs((candidate - observed_at).total_seconds()))
                    quality = "estimated_year_configured_offset"
                    details["assumed_utc_offset_minutes"] = utc_offset_minutes
                    details["timestamp_flags"].append("year_ambiguous")
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
            explicit[label] = list(dict.fromkeys(matches))
    details["explicit_fields"] = explicit
    clients = _identities(text, "client")
    aps = _identities(text, "ap")
    details["client_macs"] = clients
    details["ap_macs"] = aps
    if len(clients) > 1:
        details["ambiguous_client"] = True
    event_type = "unknown"
    for name, pattern in (("roaming", r"\broam(?:ed|ing|s)?\b"),
                          ("disconnected", r"\bdisconnect(?:ed|ing|s)?\b|\bdisassociat(?:ed|ion)\b"),
                          ("connection_failed", r"\bfailed to connect\b|\bconnection failed\b"),
                          ("connected", r"\bconnected\b|\bassociated\b"),
                          ("device_offline", r"\boffline\b"),
                          ("device_restart", r"\breboot(?:ed|ing)?\b|\brestart(?:ed|ing)?\b")):
        if re.search(pattern, text, re.I):
            event_type = name
            break
    details["classification"] = "heuristic" if event_type != "unknown" else "unknown"
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
