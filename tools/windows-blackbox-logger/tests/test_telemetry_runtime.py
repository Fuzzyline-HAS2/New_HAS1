"""Independent sources must not participate in the device durable-ACK path."""
from contextlib import closing
from copy import deepcopy
import json
from pathlib import Path
import socket
import sqlite3
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from collector import Collector, load_config
from helpers import TOPIC, payload
from telemetry import TelemetryStore, TelemetryConflict
from telemetry_runtime import JsonlTailer, TelemetryRuntime, validate_config, MAX_LINE, MAX_FILE


def event(number=1, **changes):
    result = dict(schema_version=1, site_id="store-1", source="game_server", source_id="store-1-game-server",
                  event_id=str(number), occurred_utc="2026-10-24T03:00:00Z", clock_quality="unverified",
                  event_type="http_request", fields={"request_id": str(number)})
    result.update(changes)
    return result


def line(number=1, **changes):
    return json.dumps(event(number, **changes)).encode() + b"\n"


def wait_for(predicate, seconds=3):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        if predicate():
            return True
        time.sleep(0.01)
    return False


class ConfigTests(unittest.TestCase):
    def base(self):
        return dict(host="localhost", port=1883, client_id="collector", username="collector",
                    password_env="TEST_PASSWORD", database="data/blackbox.sqlite3")

    def test_old_config_unchanged_and_disabled_example_loads(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "config.json"
            path.write_text(json.dumps(self.base()))
            self.assertNotIn("telemetry", load_config(path))
        result = load_config(ROOT / "config.store1.example.json")
        self.assertFalse(result["telemetry"]["omada"]["enabled"])
        self.assertFalse(result["telemetry"]["server_jsonl"]["enabled"])
        self.assertEqual(result["telemetry"]["omada"]["expected_site_name"], "HAS1")

    def test_same_database_unknown_fields_and_string_booleans_rejected(self):
        base = self.base()
        section = {"site_id": "store-1", "database": "telemetry.sqlite3", "omada": {"enabled": False}, "server_jsonl": {"enabled": False}}
        for mutation in (dict(database=base["database"]), dict(unknown=1), dict(omada={"enabled": "false"}), dict(site_id="")):
            config = dict(base, telemetry=dict(section, **mutation))
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                validate_config(config, Path.cwd())

    def test_placeholder_server_path_cannot_enable(self):
        data = json.loads((ROOT / "config.store1.example.json").read_text())
        data["telemetry"]["server_jsonl"]["enabled"] = True
        data["telemetry"]["server_jsonl"]["directory"] = "C:/SET_GAME_SERVER_LOG_DIRECTORY"
        with self.assertRaisesRegex(ValueError, "actual game server"):
            validate_config(data, ROOT)

    def test_service_nested_serialization_and_module_copy_contract(self):
        script = (ROOT / "service.ps1").read_text()
        self.assertIn("ConvertTo-Json -Depth 12", script)
        for module in ("omada.py", "telemetry.py", "telemetry_runtime.py", "timeline.py", "incidents.py", "incident_evidence.py"):
            self.assertIn("'" + module + "'", script)
            self.assertTrue((ROOT / module).is_file())
        self.assertIn("$loaded.telemetry.database = Join-Path $StateDir 'data\\telemetry.sqlite3'", script)


class TailTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.folder = Path(self.temp.name)
        self.path = self.folder / "server-1000-run-0.jsonl"
        self.store = TelemetryStore(self.folder / "telemetry.sqlite3")
        self.tailer = JsonlTailer(self.folder, "store-1", "store-1-game-server", self.store.save)

    def tearDown(self):
        self.store.close()
        self.temp.cleanup()

    def rows(self):
        return self.store.db.execute("SELECT count(*) FROM telemetry_events").fetchone()[0]

    def test_partial_line_then_completion_and_observer_override(self):
        body = line(1, observed_utc="1999-01-01T00:00:00Z")
        self.path.write_bytes(body[:20])
        self.tailer.poll()
        self.assertEqual(self.rows(), 0)
        with self.path.open("ab") as stream:
            stream.write(body[20:])
        self.tailer.poll()
        self.assertEqual(self.rows(), 1)
        observed = self.store.db.execute("SELECT observed_utc FROM telemetry_events").fetchone()[0]
        self.assertFalse(observed.startswith("1999"))

    def test_rotation_restart_replay_idempotent(self):
        self.path.write_bytes(line(1))
        self.tailer.poll()
        (self.folder / "server-1001-run-1.jsonl").write_bytes(line(2))
        self.tailer.poll()
        replay = JsonlTailer(self.folder, "store-1", "store-1-game-server", self.store.save)
        replay.poll()
        self.assertEqual(self.rows(), 2)
        self.assertEqual(replay.counts["duplicates"], 2)

    def test_storage_fault_retains_line_for_retry(self):
        self.path.write_bytes(line(1) + line(2))
        failures = [True]
        def save(value):
            if failures.pop() if failures else False:
                raise sqlite3.OperationalError("busy")
            return self.store.save(value)
        self.tailer.save = save
        self.tailer.poll()
        self.assertEqual(self.rows(), 0)
        self.tailer.poll()
        self.assertEqual(self.rows(), 2)
        self.assertEqual(self.tailer.counts["storage_errors"], 1)

    def test_invalid_foreign_site_and_source_rejected(self):
        self.path.write_bytes(b"not json\n" + line(1, site_id="store-2") + line(2, source="omada") + line(3, source_id="wrong-server") + line(4))
        self.tailer.poll()
        self.assertEqual(self.rows(), 1)
        self.assertEqual(self.tailer.counts["invalid_rows"], 4)

    def test_conflict_is_counted_without_overwriting(self):
        self.path.write_bytes(line(1) + line(1, fields={"different": True}) + line(2))
        self.tailer.poll()
        self.assertEqual(self.rows(), 2)
        self.assertEqual(self.tailer.counts["identity_conflicts"], 1)

    def test_oversized_line_skipped_with_following_valid_record(self):
        self.path.write_bytes(b"x" * (MAX_LINE * 3) + b"\n" + line(1))
        for _ in range(4):
            self.tailer.poll()
        self.assertEqual(self.rows(), 1)
        self.assertGreater(self.tailer.counts["oversized_lines"], 0)
        self.assertLessEqual(len(self.tailer.states[str(self.path)]["pending"]), 65536 + MAX_LINE)

    def test_rows_per_cycle_bounded_and_eventually_complete(self):
        self.path.write_bytes(b"".join(line(i) for i in range(300)))
        self.tailer.poll()
        self.assertLessEqual(self.rows(), 128)
        for _ in range(5):
            self.tailer.poll()
        self.assertEqual(self.rows(), 300)

    def test_old_backlog_does_not_starve_new_live_file(self):
        self.path.write_bytes(b"".join(line(i) for i in range(500)))
        self.tailer.poll()
        (self.folder / "server-2000-run-1.jsonl").write_bytes(line(9999))
        # Round-robin first-file choice reaches the new file within one full turn.
        for _ in range(2):
            self.tailer.poll()
        self.assertEqual(self.store.db.execute("SELECT count(*) FROM telemetry_events WHERE event_id='9999'").fetchone()[0], 1)

    def test_file_count_and_size_bounded(self):
        for index in range(10):
            (self.folder / f"server-{index:04}-run-0.jsonl").write_bytes(line(index))
        oversized = self.folder / "server-9999-run-0.jsonl"
        with oversized.open("wb") as stream:
            stream.truncate(MAX_FILE + 1)
        self.tailer.poll()
        self.assertLessEqual(len(self.tailer.states), 8)
        self.assertEqual(self.tailer.counts["oversized_files"], 1)

    def test_missing_directory_retries_when_created(self):
        folder = self.folder / "later"
        tailer = JsonlTailer(folder, "store-1", "store-1-game-server", self.store.save)
        tailer.poll()
        self.assertEqual(tailer.counts["directory_errors"], 1)
        folder.mkdir()
        (folder / self.path.name).write_bytes(line())
        tailer.poll()
        self.assertEqual(self.rows(), 1)


class RuntimeTests(unittest.TestCase):
    def config(self, folder):
        return {"site_id": "store-1", "database": str(Path(folder) / "telemetry.sqlite3"),
                "omada": {"enabled": True, "listen_host": "127.0.0.1", "port": 0,
                          "source_ips": ["127.0.0.1"], "expected_site_name": "HAS1", "utc_offset_minutes": 540},
                "server_jsonl": {"enabled": False}}

    def test_disabled_runtime_does_not_open_storage(self):
        runtime = TelemetryRuntime(None, store_factory=lambda _: self.fail("opened disabled database"))
        runtime.start()
        self.assertTrue(runtime.stop())

    def test_omada_real_udp_separate_db_and_site_mismatch(self):
        with tempfile.TemporaryDirectory() as folder:
            runtime = TelemetryRuntime(self.config(folder))
            runtime.start()
            try:
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
                    address = ("127.0.0.1", runtime.receiver.port)
                    sender.sendto(b"Oct 24 12:00:00 ctrl [site:other] Client aa-bb-cc-dd-ee-ff disconnected", address)
                    sender.sendto(b"Oct 24 12:00:00 ctrl [site:HAS1] Client aa-bb-cc-dd-ee-ff disconnected", address)
                self.assertTrue(wait_for(lambda: runtime.receiver.stats()["stored"] == 1))
            finally:
                self.assertTrue(runtime.stop())
            self.assertEqual(runtime.stats()["omada"]["site_mismatch"], 1)
            with closing(sqlite3.connect(Path(folder) / "telemetry.sqlite3")) as db:
                self.assertEqual(db.execute("SELECT count(*) FROM telemetry_events WHERE source='omada'").fetchone()[0], 1)

    def test_telemetry_database_fault_does_not_delay_device_ack(self):
        with tempfile.TemporaryDirectory() as folder:
            entered, release = threading.Event(), threading.Event()
            def broken(_):
                entered.set()
                release.wait(3)
                raise sqlite3.OperationalError("test blocked telemetry DB")
            runtime = TelemetryRuntime(self.config(folder), store_factory=broken)
            runtime.start()
            runtime.receiver.submit(b"Oct 24 12:00:00 ctrl event", "127.0.0.1")
            acks = threading.Event()
            collector = Collector(Path(folder) / "blackbox.sqlite3", lambda *_: acks.set() or True)
            try:
                self.assertTrue(entered.wait(1))
                collector.start()
                self.assertTrue(collector.submit(TOPIC, payload()))
                self.assertTrue(acks.wait(1), "device ACK blocked on unrelated telemetry")
                self.assertFalse(runtime.stop(0.01))
            finally:
                release.set()
                runtime.stop()
                collector.close()
            self.assertEqual(runtime.stats()["omada"]["callback_errors"], 1)

    def test_disk_reserve_blocks_optional_storage_then_recovers(self):
        from collections import namedtuple
        Usage = namedtuple("Usage", "total used free")
        with tempfile.TemporaryDirectory() as folder:
            runtime = TelemetryRuntime(self.config(folder))
            from omada import parse_syslog
            from datetime import datetime, timezone
            source = parse_syslog(b"test", "127.0.0.1", datetime.now(timezone.utc), "store-1")
            with patch("telemetry_runtime.shutil.disk_usage", return_value=Usage(100, 99, 1)):
                with self.assertRaises(OSError):
                    runtime._omada_save(source)
                self.assertFalse(Path(runtime.config["database"]).exists())
                self.assertEqual(runtime.counts["disk_reserve_blocks"], 1)
            runtime._disk_checked = -float("inf")
            with patch("telemetry_runtime.shutil.disk_usage", return_value=Usage(10**12, 0, 10**12)):
                runtime._omada_save(source)
                runtime._omada_close()
            self.assertTrue(Path(runtime.config["database"]).is_file())

    def test_omada_initial_bind_failure_retries_without_process_restart(self):
        from omada import OmadaReceiver
        original = OmadaReceiver.start
        attempts = []
        def first_failure(receiver):
            attempts.append(1)
            if len(attempts) == 1:
                raise OSError("NIC not ready")
            return original(receiver)
        with tempfile.TemporaryDirectory() as folder, patch("telemetry_runtime.OMADA_RETRY_SECONDS", 0.01), patch.object(OmadaReceiver, "start", first_failure):
            runtime = TelemetryRuntime(self.config(folder))
            runtime.start()
            try:
                self.assertTrue(wait_for(lambda: runtime.receiver.is_receiving()))
                self.assertEqual(runtime.counts["omada_start_errors"], 1)
                self.assertEqual(runtime.counts["omada_restart_attempts"], 1)
            finally:
                self.assertTrue(runtime.stop())

    def test_fresh_database_simultaneous_sources_serialize_initialization(self):
        for attempt in range(5):
            with self.subTest(attempt=attempt), tempfile.TemporaryDirectory() as folder:
                config = self.config(folder)
                config["server_jsonl"] = {"enabled": True, "directory": folder, "source_id": "store-1-game-server", "poll_seconds": 0.1}
                (Path(folder) / "server-1000-run-0.jsonl").write_bytes(line())
                active, peak = [0], [0]
                lock = threading.Lock()
                def factory(path):
                    with lock:
                        active[0] += 1
                        peak[0] = max(peak[0], active[0])
                    try:
                        time.sleep(0.01)
                        return TelemetryStore(path)
                    finally:
                        with lock:
                            active[0] -= 1
                runtime = TelemetryRuntime(config, store_factory=factory)
                runtime.start()
                try:
                    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
                        sender.sendto(b"Oct 24 12:00:00 ctrl [site:HAS1] Client aa-bb-cc-dd-ee-ff disconnected", ("127.0.0.1", runtime.receiver.port))
                    self.assertTrue(wait_for(lambda: runtime.receiver.stats()["stored"] == 1 and runtime.tailer and runtime.tailer.counts["stored"] == 1))
                    self.assertEqual(runtime.receiver.stats()["callback_errors"], 0)
                    self.assertEqual(peak[0], 1)
                finally:
                    self.assertTrue(runtime.stop())

    def test_dead_udp_reader_recovers_and_keeps_counters(self):
        with tempfile.TemporaryDirectory() as folder, patch("telemetry_runtime.OMADA_RETRY_SECONDS", 0.01):
            runtime = TelemetryRuntime(self.config(folder))
            runtime.start()
            original = runtime.receiver
            try:
                original._socket.close()  # Simulate an unexpected receive socket fault.
                self.assertTrue(wait_for(lambda: runtime.receiver is not original and runtime.receiver.is_receiving()))
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
                    sender.sendto(b"test recovered reader", ("127.0.0.1", runtime.receiver.port))
                self.assertTrue(wait_for(lambda: runtime.stats()["omada"]["stored"] == 1))
                self.assertEqual(runtime.stats()["omada"]["socket_errors"], 1)
            finally:
                self.assertTrue(runtime.stop())

    def test_tail_thread_live_import_and_shutdown(self):
        with tempfile.TemporaryDirectory() as folder:
            config = self.config(folder)
            config["omada"] = {"enabled": False}
            config["server_jsonl"] = {"enabled": True, "directory": folder, "source_id": "store-1-game-server", "poll_seconds": 0.1}
            (Path(folder) / "server-1000-run-0.jsonl").write_bytes(line())
            runtime = TelemetryRuntime(config)
            runtime.start()
            try:
                self.assertTrue(wait_for(lambda: runtime.tailer and runtime.tailer.counts["stored"] == 1))
            finally:
                self.assertTrue(runtime.stop())


if __name__ == "__main__":
    unittest.main()
