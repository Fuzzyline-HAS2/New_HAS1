"""MQTT -> bounded queue -> SQLite FULL commit -> application ACK.

MQTT receipt/PUBACK never authorizes the device to reclaim flash records.
"""
import argparse
from collections import Counter
import json
import logging
from logging.handlers import RotatingFileHandler
import os
from pathlib import Path
import queue
import signal
import sqlite3
import sys
import threading
import time
from telemetry import utc_now

from protocol import MAX_PAYLOAD, TOPIC, ProtocolError, parse_batch
from storage import IdentityCollision, Store
from service_runtime import BrokerInstanceLock, InstanceLock, read_machine_password
from telemetry_runtime import TelemetryRuntime, validate_config as validate_telemetry_config

LOG = logging.getLogger("blackbox")


class Collector:
    def __init__(self, database, publish, queue_size=64, store_factory=Store):
        self.database = database
        self.publish = publish
        self.store_factory = store_factory
        self.pending = queue.Queue(maxsize=queue_size)
        self.counts = Counter()
        self.stop_event = threading.Event()
        self.submit_lock = threading.Lock()
        self.ready = threading.Event()
        self.start_error = None
        self.clock_probe = None
        self.last_arrival_utc = None
        self.last_commit_utc = None
        self.mqtt_status = {"connected": False, "subscribed": False, "changed_utc": utc_now(), "reason": "startup"}
        self.worker = threading.Thread(target=self._run, name="blackbox-sqlite")

    def start(self):
        self.worker.start()
        if not self.ready.wait(10):
            self.stop_event.set()
            raise RuntimeError("SQLite writer startup timed out")
        if self.start_error:
            raise RuntimeError("SQLite writer startup failed") from self.start_error

    def submit(self, topic, payload, retained=False):
        # Only inexpensive bounded checks here. CRC/SQL/fsync belong to the worker.
        if retained or len(topic) > 80 or len(payload) > MAX_PAYLOAD or not TOPIC.fullmatch(topic):
            self.counts["rejected"] += 1
            return False
        # Shutdown and acceptance share a lock: once close begins, no callback
        # can enqueue behind the writer's final empty-queue check.
        with self.submit_lock:
            if self.stop_event.is_set():
                return False
            try:
                arrival = utc_now()
                self.pending.put_nowait((topic, bytes(payload), arrival, time.monotonic()))
                self.last_arrival_utc = arrival
                self.counts["accepted_batches"] += 1
                return True
            except queue.Full:
                self.counts["queue_full"] += 1
                return False

    def process(self, store, topic, payload, arrival_utc=None, arrival_mono=None):
        batch = parse_batch(topic, payload)
        if arrival_utc is None:
            store.save(batch)
        else:
            store.save(batch, arrival_utc=arrival_utc,
                       queue_latency_ms=max(0, (time.monotonic() - arrival_mono) * 1000))
        if self.clock_probe:
            try:
                self.clock_probe.challenge(batch)
            except Exception:
                self.counts["clock_probe_errors"] += 1
        self.last_commit_utc = utc_now()
        self.counts["committed_batches"] += 1
        # A failed/queued/lost MQTT send is safe: device retains data and retries.
        try:
            result = self.publish(batch.ack_topic, batch.ack_payload)
        except Exception:
            self.counts["ack_publish_failed"] += 1
            LOG.warning("Batch committed but ACK publish failed; replay is safe", exc_info=True)
            return False
        if not result:
            self.counts["ack_publish_failed"] += 1
            return False
        self.counts["acks_queued"] += 1
        return True

    def _run(self):
        try:
            store = self.store_factory(self.database)
        except Exception as exc:
            self.start_error = exc
            self.ready.set()
            return
        self.ready.set()
        try:
            while not self.stop_event.is_set() or not self.pending.empty():
                try:
                    topic, payload, arrival_utc, arrival_mono = self.pending.get(timeout=0.2)
                except queue.Empty:
                    continue
                try:
                    self.process(store, topic, payload, arrival_utc, arrival_mono)
                except (ProtocolError, IdentityCollision) as exc:
                    self.counts["invalid_batches"] += 1
                    LOG.warning("Batch rejected without ACK: %s", exc)
                except (sqlite3.Error, OSError) as exc:
                    self.counts["storage_errors"] += 1
                    LOG.error("Persistence failed; no ACK; device must replay: %s", exc)
                except Exception:
                    # Keep processing after a transient publisher/library failure.
                    self.counts["processing_errors"] += 1
                    LOG.exception("Batch processing failed; device must replay")
                finally:
                    self.pending.task_done()
        finally:
            store.close()

    def close(self):
        with self.submit_lock:
            self.stop_event.set()
        if self.worker.ident is not None:
            self.worker.join()


def load_config(path):
    path = Path(path).resolve()
    if path.stat().st_size > 16384:
        raise ValueError("config is too large")
    config = json.loads(path.read_text(encoding="utf-8"))
    for name in ("host", "username", "password_env", "client_id", "database"):
        if not isinstance(config.get(name), str) or not 1 <= len(config[name]) <= 512:
            raise ValueError(f"config requires a nonempty {name}")
    port = config.get("port", 1883)
    if type(port) is not int or not 1 <= port <= 65535:
        raise ValueError("port must be 1..65535")
    config["port"] = port
    config["database"] = str((path.parent / config["database"]).resolve())
    if config.get("tls_ca"):
        config["tls_ca"] = str((path.parent / config["tls_ca"]).resolve())
    telemetry = validate_telemetry_config(config, path.parent)
    if telemetry is not None:
        config["telemetry"] = telemetry
    return config


def run(config, *, credential_file=None):
    import paho.mqtt.client as mqtt

    password = (read_machine_password(credential_file) if credential_file else
                os.environ.get(config["password_env"]))
    if not password:
        raise ValueError(f"set password environment variable {config['password_env']}")
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                         client_id=config["client_id"], clean_session=True,
                         protocol=mqtt.MQTTv311)
    client.username_pw_set(config["username"], password)
    if config.get("tls_ca"):
        client.tls_set(ca_certs=config["tls_ca"])
    client.max_queued_messages_set(64)
    client.max_inflight_messages_set(8)
    client.reconnect_delay_set(min_delay=5, max_delay=60)

    def publish(topic, payload):
        return client.publish(topic, payload, qos=1, retain=False).rc == mqtt.MQTT_ERR_SUCCESS

    collector = Collector(config["database"], publish)
    telemetry = TelemetryRuntime(config.get("telemetry"), device_database=config["database"],
                                 collector_stats=lambda: {"counts": dict(collector.counts),
                                     "queue_size": collector.pending.qsize(), "worker_alive": collector.worker.is_alive(),
                                     "mqtt": dict(collector.mqtt_status), "last_arrival_utc": collector.last_arrival_utc,
                                     "last_commit_completed_utc": collector.last_commit_utc,
                                     "clock_probe": dict(collector.clock_probe.counts) if collector.clock_probe else {}})
    if (config.get("telemetry") or {}).get("clock_probe", {}).get("enabled"):
        from clock_probe import ClockProbe
        collector.clock_probe = ClockProbe(config["telemetry"]["site_id"], publish, telemetry.submit_anchor)
    stop = threading.Event()

    def on_connect(client, userdata, flags, reason_code, properties):
        collector.mqtt_status.update(connected=not reason_code.is_failure, subscribed=False,
                                     changed_utc=utc_now(), reason=str(reason_code)[:128])
        if reason_code.is_failure:
            LOG.error("Broker connection refused: %s", reason_code)
            return
        result, _mid = client.subscribe("has1/blackbox/v1/+/batch", qos=1)
        if result != mqtt.MQTT_ERR_SUCCESS:
            LOG.error("Subscription request failed: %s", result)
        LOG.info("Broker connected; subscription requested")

    def on_subscribe(client, userdata, mid, reason_codes, properties):
        failed = any(reason.is_failure for reason in reason_codes)
        collector.mqtt_status.update(subscribed=not failed,changed_utc=utc_now(),reason="subscription denied" if failed else "subscribed")
        if failed:
            LOG.error("Broker denied batch subscription; check collector ACL")

    def on_message(client, userdata, message):
        if (collector.clock_probe and not message.retain and
                message.payload.startswith(b"{") and len(message.payload) <= 1024):
            collector.clock_probe.receive(message.topic, message.payload)
            return
        collector.submit(message.topic, message.payload, message.retain)

    client.on_connect = on_connect
    client.on_subscribe = on_subscribe
    client.on_message = on_message
    def on_disconnect(c, u, f, reason, p):
        collector.mqtt_status.update(connected=False,subscribed=False,changed_utc=utc_now(),reason=str(reason)[:128])
        collector.counts["mqtt_disconnects"] += 1
        LOG.warning("Broker disconnected: %s",reason)
    client.on_disconnect = on_disconnect
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stop.set())
    collector.start()
    try:
        telemetry.start()
        client.connect_async(config["host"], config["port"], keepalive=30)
        client.loop_start()
        LOG.info("Collector running; database=%s", config["database"])
        ticks = 0
        while not stop.wait(0.25):
            if not collector.worker.is_alive():
                raise RuntimeError("SQLite writer stopped unexpectedly")
            ticks += 1
            if ticks >= 120:
                LOG.info("Counters: %s; pending=%d", dict(collector.counts), collector.pending.qsize())
                if config.get("telemetry"):
                    LOG.info("Telemetry counters: %s", telemetry.stats())
                ticks = 0
    finally:
        # Independent telemetry shutdown never participates in device durable ACKs.
        telemetry.stop(timeout=5)
        # Drain accepted batches while publisher is still running.
        collector.close()
        client.disconnect()
        client.loop_stop()
        LOG.info("Collector stopped cleanly after draining accepted batches")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=Path(__file__).with_name("config.json"))
    parser.add_argument("--credential-file", type=Path, help="Windows machine-DPAPI credential file")
    parser.add_argument("--check-config", action="store_true", help="Validate configuration without connecting")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        config = load_config(args.config)
        if args.check_config:
            import paho.mqtt.client  # Also verify the selected Python environment.
            if config.get("tls_ca") and not Path(config["tls_ca"]).is_file():
                raise ValueError("tls_ca must point to an existing certificate file")
            if args.credential_file:
                read_machine_password(args.credential_file)
            return 0
        with BrokerInstanceLock(config), InstanceLock(config["database"]):
            log_path = Path(config["database"]).parent / "collector.log"
            log_path.parent.mkdir(parents=True, exist_ok=True)
            handler = RotatingFileHandler(log_path, maxBytes=1024 * 1024, backupCount=3, encoding="utf-8")
            handler.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(message)s"))
            LOG.addHandler(handler)
            try:
                return run(config, credential_file=args.credential_file)
            finally:
                LOG.removeHandler(handler)
                handler.close()
    except (ValueError, OSError, RuntimeError, sqlite3.Error) as exc:
        LOG.error("Collector stopped: %s", exc)
        return 1


if __name__ == "__main__":
    sys.exit(main())
