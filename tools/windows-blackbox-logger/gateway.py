"""Best-effort PHP evidence: loopback-only bounded UDP, independent storage worker.

Loopback/source labels are provenance constraints, not authentication against
other local processes. UDP sends are not acknowledged and losses are possible.
"""
from datetime import datetime, timezone
import errno
import json
import queue
import socket
import threading
import time

from telemetry import Event


def parse_gateway(payload, *, site_id, source_id, observed_utc):
    if not payload or len(payload) > 2048:
        raise ValueError("gateway datagram exceeds bound")
    value = json.loads(payload.decode("utf-8"))
    if not isinstance(value, dict) or value.get("schema_version") != 1:
        raise ValueError("gateway schema required")
    if (value.get("site_id") != site_id or value.get("source") != "game_gateway" or
            value.get("source_id") != source_id or
            value.get("event_type") != "gateway_request_finished"):
        raise ValueError("unexpected gateway provenance")
    # Sender clocks are not measured. Never accept an asserted clock guarantee.
    value["clock_quality"] = "unverified"
    value["uncertainty_ms"] = None
    return Event.from_dict(value, observed_utc=observed_utc)


class GatewayReceiver:
    def __init__(self, *, host="127.0.0.1", port=5515, site_id, source_id,
                 on_event, on_close=None, queue_capacity=256):
        if host != "127.0.0.1":
            raise ValueError("gateway receiver must bind IPv4 loopback")
        if not isinstance(port, int) or isinstance(port, bool) or not 0 <= port <= 65535:
            raise ValueError("invalid UDP port")
        if site_id != "store-1" or not isinstance(source_id, str) or not source_id:
            raise ValueError("explicit store-1/source required")
        if not 1 <= queue_capacity <= 4096:
            raise ValueError("invalid queue capacity")
        self.host, self.port = host, port
        self.site_id, self.source_id = site_id, source_id
        self.on_event, self.on_close = on_event, on_close
        self._queue = queue.Queue(maxsize=queue_capacity)
        self._stop = threading.Event()
        self._producer_done = threading.Event()
        self._reader = self._consumer = self._socket = None
        self._lock = threading.Lock()
        self._counts = dict(received=0, invalid=0, filtered=0, overflow=0,
                            stored=0, callback_errors=0, socket_errors=0, close_errors=0)

    def _count(self, key):
        with self._lock:
            self._counts[key] += 1

    def stats(self):
        with self._lock:
            return {**self._counts, "queue_depth": self._queue.qsize(),
                    "receiving": self.is_receiving(),
                    "consumer_alive": bool(self._consumer and self._consumer.is_alive())}

    def is_receiving(self):
        return bool(self._reader and self._reader.is_alive() and not self._stop.is_set())

    def start(self):
        if self._reader is not None:
            raise RuntimeError("gateway receiver cannot restart")
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.bind((self.host, self.port))
            sock.settimeout(0.2)
        except BaseException:
            sock.close()
            raise
        self._socket = sock
        self.port = sock.getsockname()[1]
        self._reader = threading.Thread(target=self._receive, name="gateway-udp", daemon=True)
        self._consumer = threading.Thread(target=self._consume, name="gateway-store", daemon=True)
        self._reader.start()
        self._consumer.start()

    def _receive(self):
        try:
            while not self._stop.is_set():
                try:
                    # Extra byte detects truncation, including Windows WSAEMSGSIZE.
                    payload, address = self._socket.recvfrom(2049)
                except socket.timeout:
                    continue
                except OSError as exc:
                    if self._stop.is_set():
                        break
                    if exc.errno == errno.EMSGSIZE or getattr(exc, "winerror", None) == 10040:
                        self._count("invalid")
                        continue
                    self._count("socket_errors")
                    break
                observed = datetime.now(timezone.utc).isoformat()
                self._count("received")
                if address[0] != "127.0.0.1":
                    self._count("filtered")
                    continue
                try:
                    event = parse_gateway(payload, site_id=self.site_id,
                                          source_id=self.source_id, observed_utc=observed)
                except (ValueError, UnicodeError, RecursionError):
                    self._count("invalid")
                    continue
                try:
                    self._queue.put_nowait(event)
                except queue.Full:
                    self._count("overflow")
        finally:
            self._stop.set()
            self._producer_done.set()

    def _consume(self):
        try:
            while not self._producer_done.is_set() or not self._queue.empty():
                try:
                    event = self._queue.get(timeout=0.1)
                except queue.Empty:
                    continue
                try:
                    self.on_event(event)
                    self._count("stored")
                except Exception:
                    self._count("callback_errors")
                finally:
                    self._queue.task_done()
        finally:
            if self.on_close:
                try:
                    self.on_close()
                except Exception:
                    self._count("close_errors")

    def stop(self, timeout=5):
        deadline = time.monotonic() + max(0, timeout)
        self._stop.set()
        if self._socket:
            self._socket.close()
        for thread in (self._reader, self._consumer):
            if thread:
                thread.join(max(0, deadline - time.monotonic()))
        return not any(thread and thread.is_alive() for thread in (self._reader, self._consumer))
