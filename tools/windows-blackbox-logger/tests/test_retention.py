"""Retention keeps a full recent window, then only problem evidence, then nothing."""
from contextlib import closing
from datetime import datetime, timedelta, timezone
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from helpers import MAC as DEVICE_MAC, TOPIC, payload, record
from evidence_runtime import DeviceImporter
from incidents import IncidentStore
from protocol import parse_batch
import retention
from retention import add_keep, enable_shrink, list_keeps, prune, pruned_before, remove_keep
from storage import Store
from telemetry import Event, TelemetryStore

# Real time: the importer and marker checks rightly treat markers in the future as a wrong clock.
NOW = datetime.now(timezone.utc).replace(microsecond=0)
GLOVE = "A4F00FD90898"
OTHER = "A4F00FD908BC"


def at(days=0, minutes=0, hours=0, now=NOW):
    return (now - timedelta(days=days, hours=hours, minutes=minutes)).isoformat(timespec="microseconds")


class RetentionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.folder = Path(self.temp.name)
        self.path = self.folder / "telemetry.sqlite3"
        self.incidents = self.folder / "incidents.sqlite3"
        self.store = TelemetryStore(self.path)
        self.count = 0
        # Incident 10 days ago: G2P7 offline for one minute during a game.
        self.incident(GLOVE, "G2P7", at(days=10), at(days=10, minutes=-1))

    def tearDown(self):
        self.store.close()
        self.temp.cleanup()

    def incident(self, mac, device, opened, updated, state="recovered", cursor="1000000000"):
        store = IncidentStore(self.incidents)
        with store.transaction():
            store.db.execute("INSERT INTO incident_state VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                             ("incident-%s-%s" % (device, opened), "store-1", "server", "boot", "1652",
                              device, mac, state, "reconnected", opened, updated, "{}"))
            store.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('cursor',?)", (cursor,))
        store.close()

    def save(self, source, kind, when, mac=None, device=None, observed=None, fields=None):
        self.count += 1
        self.store.save(Event(site_id="store-1", source=source, source_id=source + "-1",
                              event_id="e%d" % self.count, observed_utc=observed or when,
                              occurred_utc=None if source == "device" else when, mac=mac,
                              device_id=device, event_type=kind, fields=fields or {}))
        return "e%d" % self.count

    def remaining(self):
        return {row[0] for row in self.store.db.execute("SELECT event_id FROM telemetry_events")}

    def apply(self, **options):
        return prune(self.path, "store-1", incidents_path=self.incidents, apply=True, now=NOW, **options)

    def test_tiers_full_window_evidence_and_expiry(self):
        recent = self.save("game_server", "request_finish", at(days=2), GLOVE, "G2P7")
        old_request = self.save("game_server", "request_finish", at(days=9), GLOVE, "G2P7")
        old_snapshot = self.save("game_server", "game_monitor_snapshot", at(days=9))
        alarm = self.save("game_server", "device_offline", at(days=9), GLOVE, "G2P7")
        omada = self.save("omada", "client_disconnected", at(days=9), GLOVE)
        crash = self.save("device", "Crash", at(days=9), DEVICE_MAC)
        health = self.save("device", "Health", at(days=9), DEVICE_MAC)
        # Inside the incident window (10 days ago, +/- 10 minutes).
        snapshot = self.save("game_server", "game_monitor_snapshot", at(days=10, minutes=5))
        own_request = self.save("game_server", "request_finish", at(days=10, minutes=5), GLOVE, "G2P7")
        by_key = self.save("game_server", "request_ingress", at(days=10, minutes=5), None, "G2P7")
        gateway = self.save("game_gateway", "gateway_request_finished", at(days=10, minutes=5), GLOVE, "G2P7")
        other_request = self.save("game_server", "request_finish", at(days=10, minutes=5), OTHER, "G2P8")
        late_upload = self.save("device", "Progress", at(days=10, hours=-5), GLOVE)
        pre_game = self.save("device", "Progress", at(days=10, hours=1), GLOVE)
        too_late = self.save("device", "Progress", at(days=10, hours=-8), GLOVE)
        other_device = self.save("device", "Progress", at(days=10, hours=-1), OTHER)
        expired = self.save("game_server", "device_offline", at(days=400), GLOVE, "G2P7")

        dry = prune(self.path, "store-1", incidents_path=self.incidents, now=NOW)
        self.assertEqual(dry["would_delete"], 7)
        self.assertEqual(len(self.remaining()), 17)

        result = self.apply(page=3)
        self.assertEqual(result["deleted"], 7)
        self.assertEqual(self.remaining(), {recent, alarm, omada, crash, snapshot, own_request, by_key, gateway,
                                            late_upload, pre_game})
        for gone in (old_request, old_snapshot, health, other_request, too_late, other_device, expired):
            self.assertNotIn(gone, self.remaining())
        self.assertEqual(result["kept"]["incident"], 6)
        self.assertEqual(result["kept_server_types"], {"device_offline": 1})
        # Every derived table loses exactly the same identities.
        for table in ("event_times", "telemetry_ingest"):
            self.assertEqual(self.store.db.execute("SELECT count(*) FROM " + table).fetchone()[0], 10)
        with closing(sqlite3.connect(self.path)) as db:
            self.assertEqual(pruned_before(db, "store-1"), NOW - timedelta(days=7))

    def test_evidence_expiry_overrides_incidents_and_keep_ranges(self):
        self.incident(GLOVE, "G2P7", at(days=400), at(days=400, minutes=-1))
        add_keep(self.path, "store-1", at(days=401), at(days=399), None, "old complaint", now=NOW)
        self.save("game_server", "request_finish", at(days=400), GLOVE, "G2P7")
        self.save("game_server", "device_offline", at(days=400), GLOVE, "G2P7")
        self.apply()
        self.assertEqual(self.remaining(), set())

    def test_open_incident_window_is_capped(self):
        self.incident(OTHER, "G2P8", at(days=20), at(days=20), state="open")
        inside = self.save("game_server", "game_monitor_snapshot", at(days=20, hours=-1))
        beyond = self.save("game_server", "game_monitor_snapshot", at(days=20, hours=-3))
        result = self.apply()
        self.assertIn(inside, self.remaining())
        self.assertNotIn(beyond, self.remaining())
        self.assertEqual(result["long_open_incidents"], 1)

    def test_never_deletes_past_the_incident_worker_cursor(self):
        old = self.save("game_server", "request_finish", at(days=9), OTHER, "G2P8")
        unread = self.save("game_server", "request_finish", at(days=9), OTHER, "G2P8")
        cursor = self.store.db.execute("SELECT seq FROM telemetry_ingest WHERE event_id=?", (old,)).fetchone()[0]
        store = IncidentStore(self.incidents)
        store.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('cursor',?)", (str(cursor),))
        store.close()
        result = self.apply()
        self.assertEqual(self.remaining(), {unread})
        self.assertEqual(result["kept"], {"awaiting_incident_worker": 1})
        # Once the worker catches up, the next ordinary run classifies the held-back row.
        store = IncidentStore(self.incidents)
        store.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('cursor','1000000000')")
        store.close()
        self.assertFalse(self.apply()["full_scan"])
        self.assertEqual(self.remaining(), set())
        # Without the worker running, past incidents still protect but the cursor does not.
        held = self.save("game_server", "request_finish", at(days=9), OTHER, "G2P8")
        store = IncidentStore(self.incidents)
        store.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('cursor','0')")
        store.close()
        prune(self.path, "store-1", incidents_path=self.incidents, protect_cursor=False, apply=True, now=NOW, rescan=True)
        self.assertNotIn(held, self.remaining())
        # New rows keep monotonically increasing ingest numbers after deletes.
        self.save("game_server", "request_finish", at(days=0))
        seqs = [row[0] for row in self.store.db.execute("SELECT seq FROM telemetry_ingest ORDER BY seq")]
        self.assertGreater(seqs[-1], cursor + 1)

    def test_operator_keep_ranges_cover_devices_and_names(self):
        kept = self.save("game_server", "request_finish", at(days=30), OTHER, "G2P8")
        by_name = self.save("game_server", "request_ingress", at(days=30), None, "G2P8")
        uploaded = self.save("device", "Progress", at(days=30, hours=-4), OTHER)
        dropped = self.save("game_server", "request_finish", at(days=30), GLOVE, "G2P7")
        first = add_keep(self.path, "store-1", at(days=30, minutes=1), at(days=30, minutes=-1), OTHER, "complaint", "G2P8", now=NOW)
        mistake = add_keep(self.path, "store-1", at(days=40), at(days=20), None, "too wide", now=NOW)
        for bad in ((at(days=1), at(days=2), None, "reversed"), ("2026-11-01T10:00:00", at(days=0), None, "no offset"),
                    (at(days=2), at(days=1), None, " ")):
            with self.assertRaises(ValueError):
                add_keep(self.path, "store-1", *bad)
        remove_keep(self.path, "store-1", mistake)
        with self.assertRaises(ValueError):
            remove_keep(self.path, "store-1", mistake)
        self.assertEqual([(k["id"], k["device"]) for k in list_keeps(self.path, "store-1")], [(first, "G2P8")])
        result = self.apply()
        self.assertEqual(self.remaining() & {kept, by_name, uploaded, dropped}, {kept, by_name, uploaded})
        self.assertEqual(result["kept"]["manual_keep"], 3)

    def test_dry_run_is_read_only_and_rejects_bad_input(self):
        self.save("game_server", "request_finish", at(days=9))
        self.store.close()
        wal = Path(str(self.path) + "-wal")
        before = (self.path.read_bytes(), wal.read_bytes() if wal.exists() else b"")
        prune(self.path, "store-1", incidents_path=self.incidents, now=NOW)
        self.assertEqual((self.path.read_bytes(), wal.read_bytes() if wal.exists() else b""), before)
        with closing(sqlite3.connect(self.path)) as db:
            tables = {row[0] for row in db.execute("SELECT name FROM sqlite_master")}
        self.assertFalse(tables & {"retention_keep", "retention_state"})
        self.store = TelemetryStore(self.path)
        for full, evidence in ((0, 365), (30, 30)):
            with self.assertRaises(ValueError):
                prune(self.path, "store-1", now=NOW, full_days=full, evidence_days=evidence)
        with self.assertRaises(ValueError):
            prune(self.folder / "missing.sqlite3", "store-1", now=NOW)
        with self.assertRaises(ValueError):
            prune(self.path, "store-1", incidents_path=self.folder / "missing-incidents.sqlite3", now=NOW)

    def test_later_runs_classify_only_new_rows_with_weekly_full_scan(self):
        for _ in range(5):
            self.save("game_server", "device_offline", at(days=9), GLOVE, "G2P7")
        first = self.apply()
        self.assertEqual((first["full_scan"], first["examined"]), (True, 5))
        second = self.apply()
        self.assertEqual((second["full_scan"], second["examined"]), (False, 0))
        self.assertEqual(self.apply(rescan=True)["examined"], 5)
        # A late arrival observed before the last cutoff is caught by the weekly full scan.
        late = self.save("game_server", "request_finish", at(days=20), OTHER, "G2P8")
        self.assertEqual(self.apply()["examined"], 0)
        self.assertIn(late, self.remaining())
        self.store.db.execute("UPDATE retention_state SET value=? WHERE key='last_full_scan'", (at(days=8),))
        self.assertTrue(self.apply()["full_scan"])
        self.assertNotIn(late, self.remaining())

    def test_interrupted_run_is_redone(self):
        rows = [self.save("game_server", "request_finish", at(days=9, minutes=i), OTHER, "G2P8") for i in range(10)]
        def fail_after_first_page(report):
            if report["examined"] >= 3:
                raise RuntimeError("killed")
        with self.assertRaises(RuntimeError):
            self.apply(page=3, progress=fail_after_first_page)
        self.assertEqual(len(self.remaining()), 7)
        with closing(sqlite3.connect(self.path)) as db:
            self.assertNotIn("classified_through", retention.retention_state(db, "store-1", NOW))
        self.apply()
        self.assertEqual(self.remaining() & set(rows), set())

    def test_clock_jump_blocks_apply_and_future_marker_is_ignored(self):
        self.save("collector", "clock_step", at(hours=2), fields={"pc_clock_step_ms": 1500})
        self.save("game_server", "request_finish", at(days=9))
        self.assertEqual(self.apply()["deleted"], 1)  # Routine time sync does not block.
        self.save("collector", "clock_step", at(hours=1), fields={"pc_clock_step_ms": -7200000})
        self.save("game_server", "request_finish", at(days=9))
        with self.assertRaises(ValueError):
            self.apply()
        self.store.db.execute("UPDATE retention_state SET value=? WHERE key='pruned_before'", (at(days=-30),))
        with closing(sqlite3.connect(self.path)) as db:
            self.assertIsNone(pruned_before(db, "store-1"))  # Future marker: ignored.
        result = self.apply(allow_clock_step=True, rescan=True)
        self.assertEqual(result["deleted"], 1)
        with closing(sqlite3.connect(self.path)) as db:
            self.assertEqual(pruned_before(db, "store-1"), NOW - timedelta(days=7))

    def test_wal_stays_small_while_another_connection_is_open(self):
        for _ in range(3000):
            self.save("game_server", "request_finish", at(days=9), OTHER, "G2P8")
        result = self.apply(page=200)
        self.assertEqual(result["deleted"], 3000)
        self.assertLess(result["storage"]["after"]["wal_mb"], 2)

    def test_importer_skips_only_records_retention_pruned(self):
        raw = Store(self.folder / "raw.sqlite3")
        try:
            # Health records (kind 9) are routine; Crash (kind 2) is always kept for a year.
            raw.save(parse_batch(TOPIC, payload(records=[record(1, kind=9), record(2, kind=9)])))
            raw.db.execute("UPDATE records SET received_utc=? WHERE seq=1", (at(days=9),))
            raw.db.execute("UPDATE records SET received_utc=? WHERE seq=2", (at(days=1),))
            importer = DeviceImporter(raw.path, self.store, "store-1")
            importer.poll(); importer.poll()
            self.assertEqual(self.store.event_count("store-1"), 2)
            self.apply()
            self.assertEqual(self.store.event_count("store-1"), 1)
            for _ in range(4):
                importer.poll()
            self.assertEqual(self.store.event_count("store-1"), 1)
            self.assertGreaterEqual(importer.counts["pruned_skipped"], 1)
            # Old records the importer never reached are not pruned records: import them.
            raw.save(parse_batch(TOPIC, payload(batch="00000002", records=[record(3, kind=9), record(4, kind=2)])))
            raw.db.execute("UPDATE records SET received_utc=? WHERE seq IN (3,4)", (at(days=9),))
            for _ in range(4):
                importer.poll()
            self.assertEqual(self.store.event_count("store-1"), 3)
        finally:
            raw.close()

    def test_enable_shrink_checks_service_and_space_then_returns_space(self):
        for _ in range(3000):
            self.save("game_server", "request_finish", at(days=9), OTHER, "G2P8")
        self.store.close()
        with patch.object(retention, "collector_running", return_value=True):
            with self.assertRaises(ValueError):
                enable_shrink(self.path)
        with patch.object(retention, "collector_running", return_value=False):
            with patch.object(retention.shutil, "disk_usage", return_value=type("U", (), {"free": 1})()):
                with self.assertRaises(ValueError):
                    enable_shrink(self.path)
            converted = enable_shrink(self.path)
            self.assertTrue(converted["changed"])
            self.assertFalse(enable_shrink(self.path)["changed"])
        result = prune(self.path, "store-1", apply=True, now=NOW)
        self.assertEqual(result["deleted"], 3000)
        storage = result["storage"]
        self.assertEqual(storage["after"]["auto_vacuum"], "incremental")
        self.assertEqual(storage["after"]["free_mb"], 0)
        self.assertLess(storage["after"]["disk_mb"], storage["before"]["disk_mb"])
        self.store = TelemetryStore(self.path)


if __name__ == "__main__":
    unittest.main()
