"""Optional Omada and local game-server ingestion, outside the MQTT ACK worker."""
from collections import Counter
import ipaddress
import json
import logging
import os
import shutil
from pathlib import Path
import threading
import time
import queue
import uuid
from telemetry import parse_utc

from omada import OmadaReceiver, to_telemetry
from telemetry import Event, TelemetryStore, TelemetryConflict, utc_now

LOG = logging.getLogger("blackbox")
MAX_LINE = 40960
MAX_FILE = 20 * 1024 * 1024
READ_BUDGET = 256 * 1024
ROW_BUDGET = 128
OMADA_RETRY_SECONDS = 5.0


def _boolean(data, name, default=False):
    value = data.get(name, default)
    if type(value) is not bool:
        raise ValueError("telemetry " + name + " must be boolean")
    return value


def _keys(data, allowed, label):
    if not isinstance(data, dict) or set(data) - set(allowed):
        raise ValueError(label + " contains unknown fields or is not an object")


def validate_config(config, base_path):
    """Normalize optional configuration; absent extras preserve original behavior."""
    value = config.get("telemetry")
    if value is None:
        return None
    _keys(value, ("site_id", "database", "omada", "server_jsonl", "min_free_bytes", "device_import", "clock_probe", "gateway_udp"), "telemetry")
    site = value.get("site_id")
    if not isinstance(site, str) or not site.strip() or len(site) > 128 or "\x00" in site:
        raise ValueError("telemetry requires site_id")
    database = value.get("database")
    if not isinstance(database, str) or not database.strip() or len(database) > 1024:
        raise ValueError("telemetry requires separate database")
    database = str((Path(base_path) / database).resolve())
    if os.path.normcase(database) == os.path.normcase(str(Path(config["database"]).resolve())):
        raise ValueError("telemetry database must differ from device ACK database")
    reserve = value.get("min_free_bytes", 1024 ** 3)
    if type(reserve) is not int or not 16 * 1024 ** 2 <= reserve <= 1024 ** 4:
        raise ValueError("telemetry min_free_bytes must be 16 MiB..1 TiB")
    result = {"site_id": site, "database": database, "min_free_bytes": reserve}
    omada = value.get("omada", {"enabled": False})
    _keys(omada, ("enabled", "listen_host", "port", "source_ips", "expected_site_name", "utc_offset_minutes"), "omada")
    normalized = dict(omada)
    normalized["enabled"] = _boolean(omada, "enabled")
    if normalized["enabled"] or len(omada) > 1:
        normalized["listen_host"] = str(ipaddress.IPv4Address(omada.get("listen_host", "127.0.0.1")))
        port = omada.get("port", 5514)
        if type(port) is not int or not 1 <= port <= 65535:
            raise ValueError("Omada port must be 1..65535")
        normalized["port"] = port
        ips = omada.get("source_ips")
        if not isinstance(ips, list) or not 1 <= len(ips) <= 32 or any(not isinstance(ip, str) for ip in ips):
            raise ValueError("Omada requires 1..32 explicit source IPs")
        normalized["source_ips"] = list(dict.fromkeys(str(ipaddress.IPv4Address(ip)) for ip in ips))
        name = omada.get("expected_site_name")
        if not isinstance(name, str) or not name.strip() or len(name) > 128 or "\x00" in name:
            raise ValueError("Omada requires verified expected_site_name")
        normalized["expected_site_name"] = name
        offset = omada.get("utc_offset_minutes", 540)
        if type(offset) is not int or not -1439 <= offset <= 1439:
            raise ValueError("Omada utc_offset_minutes invalid")
        normalized["utc_offset_minutes"] = offset
    result["omada"] = normalized
    server = value.get("server_jsonl", {"enabled": False})
    _keys(server, ("enabled", "directory", "source_id", "poll_seconds"), "server_jsonl")
    normalized = dict(server)
    normalized["enabled"] = _boolean(server, "enabled")
    if normalized["enabled"] or len(server) > 1:
        directory = server.get("directory")
        source = server.get("source_id")
        if not isinstance(directory, str) or not directory.strip() or len(directory) > 1024:
            raise ValueError("server_jsonl requires directory")
        if normalized["enabled"] and ("SET_" in directory or "<" in directory or ">" in directory):
            raise ValueError("set actual game server log directory before enabling")
        if not isinstance(source, str) or not source.strip() or len(source) > 256 or "\x00" in source:
            raise ValueError("server_jsonl requires source_id")
        poll = server.get("poll_seconds", 0.5)
        if isinstance(poll, bool) or not isinstance(poll, (float, int)) or not 0.1 <= poll <= 60:
            raise ValueError("server_jsonl poll_seconds must be 0.1..60")
        normalized.update(directory=str((Path(base_path) / directory).resolve()), source_id=source, poll_seconds=poll)
    result["server_jsonl"] = normalized
    for name in ("device_import", "clock_probe"):
        settings = value.get(name, {"enabled": False})
        _keys(settings, ("enabled",), name)
        result[name] = {"enabled": _boolean(settings, "enabled")}
    gateway = value.get("gateway_udp", {"enabled": False})
    _keys(gateway, ("enabled", "host", "port", "source_id"), "gateway_udp")
    result["gateway_udp"] = {"enabled": _boolean(gateway, "enabled")}
    if result["gateway_udp"]["enabled"] and site != "store-1":
        raise ValueError("gateway_udp is bound to store-1")
    if result["gateway_udp"]["enabled"] or len(gateway)>1:
        if gateway.get("host", "127.0.0.1") != "127.0.0.1":
            raise ValueError("gateway_udp host must be IPv4 loopback")
        port = gateway.get("port",5515)
        source = gateway.get("source_id")
        if type(port) is not int or not 1 <= port <= 65535 or not isinstance(source,str) or not 1 <= len(source) <= 256 or "\x00" in source:
            raise ValueError("gateway_udp requires port and explicit source_id")
        result["gateway_udp"].update(host="127.0.0.1",port=port,source_id=source)
    return result


class JsonlTailer:
    """Read unique rotating server-*.jsonl files; replay after restart is idempotent.

    Opening per cycle allows Windows rotation. Event IDs, not line offsets, are
    durable deduplication keys. Partial lines wait for completion. DB failures keep
    their line pending for retry. No credentials/request bodies are printed.
    """
    def __init__(self, directory, site_id, source_id, save, max_files=8):
        self.directory = Path(directory)
        self.site_id, self.source_id, self.save = site_id, source_id, save
        self.max_files = max_files
        self.states = {}
        self._next_file = 0
        self.counts = Counter()

    def _files(self):
        found = []
        try:
            with os.scandir(self.directory) as entries:
                for number, entry in enumerate(entries):
                    if number >= 4096:
                        self.counts["directory_scan_limit"] += 1
                        break
                    if entry.name.startswith("server-") and entry.name.endswith(".jsonl") and entry.is_file(follow_symlinks=False):
                        found.append(Path(entry.path))
        except OSError:
            self.counts["directory_errors"] += 1
        return sorted(found, key=lambda path: path.name)[-self.max_files:]

    def poll(self):
        remaining_bytes, remaining_rows = READ_BUDGET, ROW_BUDGET
        activity = False
        paths = self._files()
        live = {str(path) for path in paths}
        for key in list(self.states):
            if key not in live:
                state = self.states.pop(key)
                if state["pending"]:
                    self.counts["retired_partial_or_pending"] += 1
        # Rotate the first file each cycle: old restart replay must not starve
        # newly created live files behind a multi-file backlog.
        if paths:
            first = self._next_file % len(paths)
            self._next_file = (first + 1) % len(paths)
            paths = paths[first:] + paths[:first]
        for path in paths:
            if remaining_bytes <= 0 or remaining_rows <= 0:
                break
            key = str(path)
            try:
                with path.open("rb") as stream:
                    stat = os.fstat(stream.fileno())
                    if stat.st_size > MAX_FILE:
                        self.counts["oversized_files"] += 1
                        continue
                    identity = (stat.st_dev, stat.st_ino)
                    state = self.states.get(key)
                    if state is None or state["identity"] != identity or stat.st_size < state["offset"]:
                        state = {"identity": identity, "offset": 0, "pending": b"", "discarding": False}
                        self.states[key] = state
                    # Process pending rows before reading another bounded chunk.
                    if b"\n" not in state["pending"]:
                        stream.seek(state["offset"])
                        chunk = stream.read(min(65536, remaining_bytes))
                        remaining_bytes -= len(chunk)
                        activity = activity or bool(chunk)
                        state["offset"] += len(chunk)
                        state["pending"] += chunk
                    while b"\n" in state["pending"] and remaining_rows > 0:
                        line, rest = state["pending"].split(b"\n", 1)
                        remaining_rows -= 1
                        activity = True
                        if state["discarding"] or len(line) > MAX_LINE:
                            self.counts["oversized_lines"] += 1
                            state["discarding"] = False
                            state["pending"] = rest
                            continue
                        try:
                            value = json.loads(line)
                            if not isinstance(value, dict) or value.get("site_id") != self.site_id or value.get("source") != "game_server" or value.get("source_id") != self.source_id:
                                raise ValueError("server source/site does not match configured identity")
                            event = Event.from_dict(value, observed_utc=utc_now())
                        except (ValueError, TypeError, UnicodeError, RecursionError, OverflowError):
                            self.counts["invalid_rows"] += 1
                            state["pending"] = rest
                            continue
                        try:
                            inserted = self.save(event)
                        except TelemetryConflict:
                            self.counts["identity_conflicts"] += 1
                            state["pending"] = rest
                            continue
                        except Exception:
                            self.counts["storage_errors"] += 1
                            return False  # Retry after idle interval, not a busy loop.
                        self.counts["stored" if inserted else "duplicates"] += 1
                        state["pending"] = rest
                    if b"\n" not in state["pending"] and len(state["pending"]) > MAX_LINE:
                        state["discarding"] = True
                        state["pending"] = b""
            except OSError:
                self.counts["file_errors"] += 1
        return activity


class TelemetryRuntime:
    def __init__(self, config, store_factory=TelemetryStore, *, device_database=None, collector_stats=None):
        self.config = config
        self.device_database = device_database
        self.collector_stats = collector_stats or (lambda: {})
        self.gateway = None
        self._gateway_previous = Counter()
        self._gateway_checked = 0
        self._gateway_store = None
        self._evidence_thread = None
        self._anchors = queue.Queue(maxsize=64)
        self._run_id = uuid.uuid4().hex
        self.importer = None
        self.store_factory = store_factory
        self.receiver = None
        self.tailer = None
        self._stop = threading.Event()
        self._tail_thread = None
        self._omada_monitor = None
        self._omada_previous = Counter()
        self.counts = Counter()
        self._omada_store = None
        self._disk_lock = threading.Lock()
        self._store_init_lock = threading.Lock()
        self._disk_checked = -float("inf")
        self._disk_free = 0

    def _check_disk(self):
        # Sample at most once a second; this is a safety margin, not a reservation
        # against other processes consuming the same physical disk.
        with self._disk_lock:
            now = time.monotonic()
            if now - self._disk_checked >= 1:
                folder = Path(self.config["database"]).parent
                while not folder.exists() and folder != folder.parent:
                    folder = folder.parent
                try:
                    self._disk_free = shutil.disk_usage(folder).free
                    self._disk_checked = now
                except OSError:
                    self.counts["disk_check_errors"] += 1
                    raise
            if self._disk_free < self.config.get("min_free_bytes", 1024 ** 3):
                self.counts["disk_reserve_blocks"] += 1
                raise OSError("optional telemetry disk reserve reached")

    def _open_store(self):
        # First-open WAL/schema/application-id setup must finish before another
        # source opens the same fresh database. Connections remain thread-owned.
        with self._store_init_lock:
            return self.store_factory(self.config["database"])

    def _omada_save(self, event):
        self._check_disk()
        if self._omada_store is None:
            self._omada_store = self._open_store()
        self._omada_store.save(to_telemetry(event))

    def _omada_close(self):
        if self._omada_store is not None:
            self._omada_store.close()
            self._omada_store = None

    def _start_omada(self):
        omada = self.config["omada"]
        self.receiver = OmadaReceiver(omada["listen_host"], omada["port"],
            {ip: self.config["site_id"] for ip in omada["source_ips"]}, self._omada_save,
            utc_offset_minutes=omada["utc_offset_minutes"], on_close=self._omada_close,
            expected_site_name=omada["expected_site_name"])
        try:
            self.receiver.start()
            LOG.info("Omada UDP listener running on %s:%s", omada["listen_host"], self.receiver.port)
            return True
        except OSError:
            self.counts["omada_start_errors"] += 1
            LOG.error("Omada listener failed to bind; retry scheduled, MQTT remains active")
            return False

    def _monitor_omada(self):
        delay = OMADA_RETRY_SECONDS
        while not self._stop.wait(delay):
            if self.receiver.is_receiving():
                delay = OMADA_RETRY_SECONDS
                continue
            # Finish the previous consumer before creating another SQLite owner.
            if not self.receiver.stop(timeout=0.2):
                self.counts["omada_restart_waits"] += 1
                delay = 30
                continue
            for name, value in self.receiver.stats().items():
                if name != "queue_size":
                    self._omada_previous[name] += value
            if self._stop.is_set():
                break
            self.counts["omada_restart_attempts"] += 1
            delay = OMADA_RETRY_SECONDS if self._start_omada() else min(delay * 2, 30)

    def start(self):
        if not self.config:
            return
        if self.config.get("gateway_udp", {}).get("enabled"):
            self._start_gateway()
        self._evidence_thread = threading.Thread(target=self._evidence_run, name="evidence-maintenance", daemon=True)
        self._evidence_thread.start()
        if self.config["omada"]["enabled"]:
            self._start_omada()
            self._omada_monitor = threading.Thread(target=self._monitor_omada, name="omada-monitor", daemon=True)
            self._omada_monitor.start()
        server = self.config["server_jsonl"]
        if server["enabled"]:
            self._tail_thread = threading.Thread(target=self._tail_run, name="server-jsonl", daemon=True)
            self._tail_thread.start()

    def _start_gateway(self):
        from gateway import GatewayReceiver
        gateway = self.config["gateway_udp"]
        self.gateway = GatewayReceiver(host=gateway["host"],port=gateway["port"],site_id=self.config["site_id"],source_id=gateway["source_id"],on_event=self._gateway_save,on_close=self._gateway_close)
        try:
            self.gateway.start()
        except OSError:
            self.counts["gateway_start_errors"] += 1
            LOG.error("Gateway UDP bind failed; retry scheduled independently of MQTT")
        self._gateway_checked = time.monotonic()

    def _retry_gateway(self):
        if self._stop.is_set() or not self.gateway or time.monotonic()-self._gateway_checked < 5:
            return
        self._gateway_checked = time.monotonic()
        if self.gateway.is_receiving() or not self.gateway.stop(timeout=0):
            return
        for key,value in self.gateway.stats().items():
            if key not in ("queue_depth","receiving","consumer_alive"):
                self._gateway_previous[key] += value
        if not self._stop.is_set():
            self.counts["gateway_restarts"] += 1
            self._start_gateway()

    def _gateway_save(self, event):
        self._check_disk()
        if self._gateway_store is None:
            self._gateway_store = self._open_store()
        self._gateway_store.save(event)

    def _gateway_close(self):
        if self._gateway_store is not None:
            self._gateway_store.close()
            self._gateway_store = None

    def submit_anchor(self, anchor):
        try:
            self._anchors.put_nowait(anchor)
            return True
        except queue.Full:
            self.counts["anchor_queue_full"] += 1
            return False

    def _evidence_run(self):
        from evidence_runtime import DeviceImporter
        store = None
        sequence = 0
        last_health = -float("inf")
        previous_wall, previous_mono = utc_now(), time.monotonic()
        pending_anchor = None
        started = False
        try:
            while not self._stop.is_set():
                try:
                    self._retry_gateway()
                    self._check_disk()
                    if store is None:
                        store = self._open_store()
                    if self.config.get("device_import", {}).get("enabled") and self.device_database:
                        if self.importer is None:
                            self.importer = DeviceImporter(self.device_database, store, self.config["site_id"])
                        try:
                            self.importer.poll()
                        except Exception:
                            self.counts["device_import_errors"] += 1
                    for _ in range(8):
                        if pending_anchor is None:
                            try:
                                pending_anchor = self._anchors.get_nowait()
                            except queue.Empty:
                                break
                        store.save_anchor(pending_anchor)
                        pending_anchor = None
                    mono, wall = time.monotonic(), utc_now()
                    step = (parse_utc(wall)-parse_utc(previous_wall)).total_seconds()-(mono-previous_mono)
                    if mono-last_health>=30 or abs(step)>1:
                        sequence += 1
                        fields = {"runtime": self.stats(), "device_collector": self.collector_stats(),
                                  "free_bytes": self._disk_free, "pc_clock_step_ms": round(step*1000,3),
                                  "enabled_sources": {name: bool(self.config.get(name,{}).get("enabled")) for name in ("omada","server_jsonl","gateway_udp","device_import","clock_probe")},
                                  "coverage_warning": "heartbeat is not proof of zero loss; no heartbeat during PC/process outage"}
                        if self.importer:
                            fields["device_import"] = self.importer.coverage()
                        store.save(Event(site_id=self.config["site_id"],source="collector",source_id="windows-collector",event_id=self._run_id+":"+str(sequence),observed_utc=wall,occurred_utc=wall,clock_quality="pc_unverified",event_type="collector_started" if not started else "clock_step" if abs(step)>1 else "collection_health",fields=fields))
                        started, last_health = True, mono
                    previous_mono, previous_wall = mono, wall
                except Exception:
                    self.counts["evidence_errors"] += 1
                    if self.counts["evidence_errors"] % 120 == 1:
                        LOG.warning("Optional evidence maintenance failed; will retry", exc_info=True)
                self._stop.wait(0.5)
        finally:
            if store:
                try:
                    self._check_disk()
                    wall = utc_now()
                    store.save(Event(site_id=self.config["site_id"],source="collector",source_id="windows-collector",event_id=self._run_id+":stop",observed_utc=wall,occurred_utc=wall,clock_quality="pc_unverified",event_type="collector_stopping",fields={"pending_anchors": self._anchors.qsize()+int(pending_anchor is not None), "runtime": self.stats()}))
                except Exception:
                    self.counts["health_stop_errors"] += 1
                store.close()

    def _tail_run(self):
        store = None
        def save(event):
            nonlocal store
            self._check_disk()
            if store is None:
                store = self._open_store()
            return store.save(event)
        settings = self.config["server_jsonl"]
        self.tailer = JsonlTailer(settings["directory"], self.config["site_id"], settings["source_id"], save)
        try:
            while not self._stop.is_set():
                active = self.tailer.poll()
                self._stop.wait(0.001 if active else settings["poll_seconds"])
        except Exception:
            self.counts["tail_worker_errors"] += 1
            LOG.exception("Server telemetry worker failed; MQTT collector remains active")
        finally:
            if store is not None:
                try:
                    store.close()
                except Exception:
                    self.counts["tail_close_errors"] += 1

    def stats(self):
        result = dict(self.counts)
        result["workers"] = {"evidence": bool(self._evidence_thread and self._evidence_thread.is_alive()),
                             "server_jsonl": bool(self._tail_thread and self._tail_thread.is_alive()),
                             "omada": bool(self.receiver and self.receiver.is_receiving()),
                             "gateway": bool(self.gateway and self.gateway.is_receiving())}
        gateway = self.gateway
        if gateway:
            result["gateway_udp"] = gateway.stats()
            for name,value in dict(self._gateway_previous).items():
                result["gateway_udp"][name] = result["gateway_udp"].get(name,0)+value
        if self.receiver:
            result["omada"] = self.receiver.stats()
            for name, value in dict(self._omada_previous).items():
                result["omada"][name] = result["omada"].get(name, 0) + value
        if self.tailer:
            result["server_jsonl"] = dict(self.tailer.counts)
        return result

    def stop(self, timeout=5):
        deadline = time.monotonic() + timeout
        self._stop.set()
        finished = True
        if self._omada_monitor:
            self._omada_monitor.join(max(0, deadline - time.monotonic()))
            finished = not self._omada_monitor.is_alive()
        if self.receiver:
            receiver_finished = self.receiver.stop(max(0, deadline - time.monotonic()))
            finished = finished and receiver_finished
        if self._tail_thread:
            self._tail_thread.join(max(0, deadline - time.monotonic()))
            finished = finished and not self._tail_thread.is_alive()
        if self._evidence_thread:
            self._evidence_thread.join(max(0,deadline-time.monotonic()))
            finished = not self._evidence_thread.is_alive() and finished
        if self.gateway:
            finished = self.gateway.stop(max(0,deadline-time.monotonic())) and finished
        if not finished:
            LOG.error("Telemetry shutdown timed out; pending telemetry may be lost")
        LOG.info("Telemetry stopped: workers_stopped=%s counters=%s", finished, self.stats())
        return finished
