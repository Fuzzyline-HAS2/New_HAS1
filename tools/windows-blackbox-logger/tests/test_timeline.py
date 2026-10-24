import io
import json
from pathlib import Path
import sqlite3
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from telemetry import (Event, TelemetryConflict, TelemetryStore, normalize_utc,
                       parse_utc)
from timeline import (Anchor, UPTIME_WRAP_MS, align_event, event_interval,
                      export_timeline, import_device_database, import_jsonl,
                      json_lines)

MAC = "AABBCCDDEEFF"
RING = "0123456789abcdef"
NOW = "2026-10-24T00:00:00+00:00"


def event(**overrides):
    values = dict(site_id="store-1", source="game_server", source_id="server-1",
                  event_id="request-1", observed_utc=NOW, occurred_utc=NOW,
                  event_type="http.request.completed")
    values.update(overrides)
    return Event(**values)


def device(**overrides):
    values = dict(site_id="store-1", source="device", source_id=MAC,
                  event_id=RING + ":10", observed_utc="2026-10-25T00:00:00Z",
                  occurred_utc=None, clock_quality="uptime_only", mac=MAC,
                  fields={"ring": RING, "boot": 3, "seq": 10, "uptime_ms": 1000})
    values.update(overrides)
    return Event(**values)


def anchor(**overrides):
    values = dict(anchor_id="measurement-1", site_id="store-1", mac=MAC, ring=RING,
                  boot=3, uptime_ms64=1000, utc_min="2026-10-24T00:00:00.000Z",
                  utc_max="2026-10-24T00:00:00.020Z", origin="USB capture of uptime with host clock measurement",
                  seq_min=10, seq_max=20, record_uptime_wraps=0, drift_ppm=1000)
    values.update(overrides)
    return Anchor(**values)


def record(seq, boot, uptime):
    raw = bytearray(64)
    raw[:2] = b"\xb7\x09"
    struct.pack_into("<IIIiiii", raw, 4, seq, boot, uptime, 10, -1, 0, 5)
    struct.pack_into("<I", raw, 60, zlib.crc32(raw[:60]))
    return bytes(raw)


class TimeNormalizationTests(unittest.TestCase):
    def test_kst_and_year_rollover_are_same_instant(self):
        self.assertEqual(normalize_utc("2027-01-01T00:10:00+09:00"),
                         normalize_utc("2026-12-31T15:10:00Z"))

    def test_dst_fold_distinguished_by_explicit_offset(self):
        earlier = parse_utc("2026-11-01T01:30:00-04:00")
        later = parse_utc("2026-11-01T01:30:00-05:00")
        self.assertEqual((later - earlier).total_seconds(), 3600)
        with self.assertRaises(ValueError):
            parse_utc("2026-11-01T01:30:00")

    def test_reject_naive_nonfinite_and_oversize_data(self):
        for change in ({"occurred_utc": "2026-10-24T09:00:00"},
                       {"uncertainty_ms": float("nan")}, {"uncertainty_ms": -1},
                       {"uncertainty_ms": True}, {"raw": "x" * 16385},
                       {"fields": {"unbounded": "x" * 16384}}, {"site_id": ""}):
            with self.subTest(change=list(change)):
                with self.assertRaises(ValueError):
                    event(**change)

    def test_timestamp_and_synced_claim_without_uncertainty_not_certified(self):
        for quality in ("unverified", "rfc5424", "synchronized"):
            self.assertIsNone(event_interval(event(clock_quality=quality).to_dict()))
        self.assertIsNone(event_interval(event(uncertainty_ms=1).to_dict()))
        self.assertIsNotNone(event_interval(event(clock_quality="synchronized", uncertainty_ms=5).to_dict()))


class AnchorTests(unittest.TestCase):
    def test_delayed_batch_arrival_is_not_device_occurrence(self):
        value = align_event(device(), [])
        self.assertIsNone(value["occurred_utc"])
        self.assertEqual(value["observed_utc"], "2026-10-25T00:00:00.000000+00:00")
        self.assertEqual(value["alignment"]["status"], "unaligned")

    def test_explicit_measured_anchor_and_drift_expansion(self):
        original = device(fields={"ring": RING, "boot": 3, "seq": 11, "uptime_ms": 101000})
        value = align_event(original, [anchor()])
        self.assertEqual(value["occurred_utc"], "2026-10-24T00:01:40.010000+00:00")
        self.assertEqual(value["uncertainty_ms"], 110)
        self.assertEqual(value["clock_quality"], "anchored")
        self.assertIsNone(original.occurred_utc)

    def test_boot_ring_site_mac_and_seq_are_all_required(self):
        for other in (anchor(boot=4), anchor(ring="fedcba9876543210"),
                      anchor(site_id="store-2"), anchor(mac="000000000001"),
                      anchor(seq_min=11)):
            with self.subTest(other=other):
                self.assertIsNone(align_event(device(), [other])["occurred_utc"])

    def test_wrap_epoch_never_inferred_from_raw_uptime(self):
        near_wrap = anchor(uptime_ms64=UPTIME_WRAP_MS + 1000, record_uptime_wraps=0)
        self.assertIsNone(align_event(device(), [near_wrap])["occurred_utc"])
        explicit = anchor(uptime_ms64=UPTIME_WRAP_MS + 1000, record_uptime_wraps=1)
        self.assertIsNotNone(align_event(device(), [explicit])["occurred_utc"])

    def test_wrap_crossing_requires_separate_explicit_epoch_range(self):
        before = device(fields={"ring": RING, "boot": 3, "seq": 10, "uptime_ms": UPTIME_WRAP_MS - 100})
        after = device(fields={"ring": RING, "boot": 3, "seq": 11, "uptime_ms": 100})
        first = anchor(uptime_ms64=UPTIME_WRAP_MS - 100, seq_max=10)
        second = anchor(anchor_id="measurement-2", uptime_ms64=UPTIME_WRAP_MS + 100,
                        seq_min=11, record_uptime_wraps=1, utc_min="2026-10-24T00:00:00.200Z",
                        utc_max="2026-10-24T00:00:00.220Z")
        self.assertIsNotNone(align_event(before, [first])["occurred_utc"])
        self.assertIsNone(align_event(after, [first])["occurred_utc"])
        self.assertEqual(align_event(after, [first, second])["occurred_utc"],
                         "2026-10-24T00:00:00.210000+00:00")

    def test_old_anchor_expiry_and_conflicting_anchors_fail_closed(self):
        self.assertIsNone(align_event(device(), [anchor(uptime_ms64=10000000)])["occurred_utc"])
        conflict = anchor(anchor_id="bad-measurement", utc_min="2026-10-24T00:01:00Z",
                          utc_max="2026-10-24T00:01:00.020Z")
        value = align_event(device(), [anchor(), conflict])
        self.assertIsNone(value["occurred_utc"])
        self.assertEqual(value["alignment"]["reason"], "conflicting measured anchors")

    def test_no_implicitly_trusted_device_timestamp(self):
        fake = device(occurred_utc=NOW, clock_quality="synchronized", uncertainty_ms=0)
        self.assertIsNone(align_event(fake, [])["occurred_utc"])

    def test_anchor_bounds_and_schema_validation(self):
        for change in ({"utc_max": "2026-10-23T00:00:00Z"}, {"max_delta_ms": 3600001},
                       {"seq_max": 9}, {"record_uptime_wraps": -1}, {"drift_ppm": float("inf")}):
            with self.subTest(change=change):
                with self.assertRaises(ValueError):
                    anchor(**change)
        with self.assertRaises(ValueError):
            Anchor.from_dict({**anchor().to_dict(), "schema_version": 2})


class StorageAndTimelineTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.store = TelemetryStore(self.root / "telemetry.sqlite3")

    def tearDown(self):
        self.store.close()
        self.temp.cleanup()

    def test_idempotence_keeps_first_arrival_and_detects_changed_facts(self):
        self.assertTrue(self.store.save(event()))
        self.assertFalse(self.store.save(event(observed_utc="2026-10-25T00:00:00Z")))
        self.assertEqual(next(self.store.events("store-1")).observed_utc, normalize_utc(NOW))
        with self.assertRaises(TelemetryConflict):
            self.store.save(event(fields={"http_status": 500}))
        self.assertEqual(self.store.event_count("store-1"), 1)
        self.store.save(event(event_id="next"))

    def test_same_mac_and_source_event_id_in_different_sites_do_not_collide(self):
        self.store.save(device())
        self.store.save(device(site_id="store-2"))
        self.assertEqual(self.store.event_count("store-1"), 1)
        self.assertEqual(self.store.event_count("store-2"), 1)

    def test_reopen_and_reject_device_ack_database(self):
        self.store.save(event())
        second = TelemetryStore(self.root / "telemetry.sqlite3")
        try:
            self.assertEqual(second.event_count("store-1"), 1)
        finally:
            second.close()
        path = self.root / "device.sqlite3"
        db = sqlite3.connect(path)
        db.execute("CREATE TABLE records(seq INTEGER)")
        db.close()
        with self.assertRaises(sqlite3.DatabaseError):
            TelemetryStore(path)

    def test_source_jsonl_import_sets_observer_time_and_checks_site_schema(self):
        path = self.root / "server.jsonl"
        source = event().to_dict()
        source.pop("observed_utc")
        path.write_text(json.dumps(source) + "\n", encoding="utf-8")
        result = import_jsonl(self.store, path, "store-1")
        self.assertEqual(result, {"inserted": 1, "duplicates": 0})
        self.assertNotEqual(next(self.store.events("store-1")).observed_utc, normalize_utc(NOW))
        self.assertEqual(import_jsonl(self.store, path, "store-1")["duplicates"], 1)
        with self.assertRaises(ValueError):
            import_jsonl(self.store, path, "store-2")
        with self.assertRaises(ValueError):
            Event.from_dict({**source, "schema_version": 2}, observed_utc=NOW)

    def make_device_database(self):
        path = self.root / "device.sqlite3"
        db = sqlite3.connect(path)
        db.execute("CREATE TABLE records(mac TEXT, ring TEXT, seq INTEGER, device TEXT, raw BLOB, received_utc TEXT)")
        db.executemany("INSERT INTO records VALUES(?,?,?,?,?,?)", [
            (MAC, RING, 10, "iotglove", record(10, 3, 1000), "2026-10-25T00:00:00Z"),
            (MAC, RING, 11, "iotglove", record(11, 4, 1000), "2026-10-26T00:00:00Z")])
        db.commit()
        db.close()
        return path

    def test_device_import_preserves_raw_and_never_backdates_arrival(self):
        path = self.make_device_database()
        before = path.read_bytes()
        self.assertEqual(import_device_database(self.store, path, "store-1")["inserted"], 2)
        values = list(self.store.events("store-1"))
        self.assertTrue(all(value.occurred_utc is None for value in values))
        self.assertEqual(bytes.fromhex(values[0].raw), record(10, 3, 1000))
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(import_device_database(self.store, path, "store-1")["duplicates"], 2)

    def test_bounded_device_import_reports_truncation_and_requires_scoped_resume(self):
        path = self.make_device_database()
        self.assertTrue(import_device_database(self.store, path, "store-1", limit=1)["truncated"])
        with self.assertRaises(ValueError):
            import_device_database(self.store, path, "store-1", after_seq=10)
        result = import_device_database(self.store, path, "store-1", after_seq=10, mac=MAC, ring=RING)
        self.assertFalse(result["truncated"])
        self.assertEqual(result["inserted"], 1)

    def test_anchor_persistence_idempotence_and_conflict(self):
        self.assertTrue(self.store.save_anchor(anchor()))
        self.assertFalse(self.store.save_anchor(anchor()))
        with self.assertRaises(TelemetryConflict):
            self.store.save_anchor(anchor(boot=4))
        self.assertEqual(len(self.store.anchor_dicts()), 1)

    def test_export_separates_known_unverified_unaligned_and_scopes_site(self):
        self.store.save(event(clock_quality="synchronized", uncertainty_ms=5))
        self.store.save(event(source="omada", event_id="omada-1", occurred_utc="2026-10-24T09:00:00+09:00"))
        self.store.save(device())
        self.store.save(event(site_id="store-2", event_id="other-store"))
        output = io.StringIO()
        self.assertEqual(export_timeline(self.store, output, "store-1", window_start=NOW, window_end=NOW), 3)
        rows = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(rows[0]["event_count_in_store"], 3)
        sections = {row["section"] for row in rows[1:]}
        self.assertEqual(sections, {"aligned", "timestamp_unverified", "unaligned"})
        self.assertEqual(rows[-1]["window_relation"], "unknown")
        self.assertEqual(next(row for row in rows[1:] if row["section"] == "aligned")["window_relation"],
                         "interval_overlap_candidate")

    def test_anchor_export_orders_by_derived_time_keeps_original_database_unaligned(self):
        self.store.save(device())
        self.store.save(event(occurred_utc="2026-10-24T00:01:00Z"))
        self.store.save_anchor(anchor())
        output = io.StringIO()
        export_timeline(self.store, output, "store-1")
        rows = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(rows[1]["event"]["source"], "device")
        self.assertEqual(rows[1]["section"], "aligned")
        self.assertIsNone(next(value for value in self.store.events("store-1") if value.source == "device").occurred_utc)

    def test_import_line_and_row_limits_and_export_truncation_visible(self):
        path = self.root / "large.jsonl"
        path.write_text("x" * 40962, encoding="utf-8")
        with self.assertRaises(ValueError):
            list(json_lines(path))
        path.write_text("{}\n{}\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            list(json_lines(path, limit=1))
        self.store.save(event())
        self.store.save(event(event_id="request-2"))
        output = io.StringIO()
        self.assertEqual(export_timeline(self.store, output, "store-1", limit=1), 1)
        self.assertTrue(json.loads(output.getvalue().splitlines()[0])["truncated"])
        with self.assertRaises(ValueError):
            export_timeline(self.store, output, "store-1", limit=100001)
        with self.assertRaises(ValueError):
            export_timeline(self.store, output, "store-1", window_start=NOW)

    def test_recent_window_is_selected_before_limit_and_device_sections_do_not_starve(self):
        for index in range(30):
            self.store.save(event(event_id="old-" + str(index), occurred_utc="2025-01-01T00:00:00Z"))
        self.store.save(event(event_id="incident"))
        self.store.save(device())
        self.store.save(device(event_id=RING + ":21", fields={"ring": RING, "boot": 4, "seq": 21, "uptime_ms": 1}))
        self.store.save_anchor(anchor())
        output = io.StringIO()
        export_timeline(self.store, output, "store-1", limit=1,
                        window_start="2026-10-23T23:59:59Z", window_end="2026-10-24T00:00:01Z")
        rows = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual({row["event"]["event_id"] for row in rows[1:]},
                         {"incident", RING + ":10", RING + ":21"})
        self.assertFalse(rows[0]["truncated"])
        self.assertEqual(rows[0]["sections"]["timestamped"]["examined"], 1)

    def test_window_query_includes_uncertainty_overlap_outside_nominal_window(self):
        self.store.save(event(occurred_utc="2026-10-24T00:00:01Z", clock_quality="measured", uncertainty_ms=1500))
        output = io.StringIO()
        self.assertEqual(export_timeline(self.store, output, "store-1", window_start=NOW, window_end=NOW), 1)
        self.assertEqual(json.loads(output.getvalue().splitlines()[1])["window_relation"], "interval_overlap_candidate")

    def test_stable_section_cursor_paging_and_context_validation(self):
        for index in range(5):
            self.store.save(event(event_id="id-" + str(index)))
        ids, cursor = [], None
        for expected in (2, 2, 1):
            output = io.StringIO()
            self.assertEqual(export_timeline(self.store, output, "store-1", limit=2, timestamped_after=cursor), expected)
            rows = [json.loads(line) for line in output.getvalue().splitlines()]
            ids.extend(row["event"]["event_id"] for row in rows[1:])
            cursor = rows[0]["sections"]["timestamped"]["next_cursor"]
            if expected == 2:
                with self.assertRaises(ValueError):
                    export_timeline(self.store, io.StringIO(), "store-2", timestamped_after=cursor)
        self.assertEqual(ids, ["id-0", "id-1", "id-2", "id-3", "id-4"])
        self.assertIsNone(cursor)

    def test_anchor_outside_window_does_not_consume_unaligned_budget(self):
        self.store.save(device())
        self.store.save(device(event_id=RING + ":21", observed_utc="2026-10-26T00:00:00Z",
                               fields={"ring": RING, "boot": 4, "seq": 21, "uptime_ms": 1}))
        self.store.save_anchor(anchor())
        output = io.StringIO()
        export_timeline(self.store, output, "store-1", limit=1,
                        window_start="2026-10-27T00:00:00Z", window_end="2026-10-27T00:01:00Z")
        rows = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(rows[1]["event"]["event_id"], RING + ":21")
        self.assertEqual(rows[1]["section"], "unaligned")

    def test_schema_one_upgrade_preserves_facts_and_builds_window_index(self):
        original = event()
        self.store.save(original)
        self.store.db.execute("DROP TABLE event_times")
        self.store.db.execute("PRAGMA user_version=1")
        reopened = TelemetryStore(self.root / "telemetry.sqlite3")
        try:
            self.assertEqual(next(reopened.events("store-1")).to_dict(), original.to_dict())
            self.assertEqual(reopened.db.execute("PRAGMA user_version").fetchone()[0], 3)
            output = io.StringIO()
            self.assertEqual(export_timeline(reopened, output, "store-1", window_start=NOW, window_end=NOW), 1)
        finally:
            reopened.close()

    def test_conflicting_anchors_stay_unaligned_outside_every_nominal_window(self):
        self.store.save(device())
        self.store.save_anchor(anchor())
        self.store.save_anchor(anchor(anchor_id="different-measurement", utc_min="2026-10-25T00:00:00Z",
                                      utc_max="2026-10-25T00:00:00.020Z"))
        output = io.StringIO()
        self.assertEqual(export_timeline(self.store, output, "store-1", limit=1,
                         window_start="2026-10-26T00:00:00Z", window_end="2026-10-26T00:01:00Z"), 1)
        rows = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertFalse(rows[0]["truncated"])
        self.assertEqual(rows[1]["section"], "unaligned")
        self.assertEqual(rows[1]["event"]["alignment"]["reason"], "conflicting measured anchors")

    def test_many_automatic_anchors_do_not_hide_latest_window(self):
        # A few hours of probes can exceed the former global 1000-anchor cap.
        from datetime import timedelta
        base = parse_utc(NOW)
        for index in range(1100):
            instant = (base + timedelta(minutes=index)).isoformat()
            self.store.save_anchor(anchor(anchor_id="auto-" + str(index),
                seq_min=index+1, seq_max=index+1, uptime_ms64=index*60000,
                utc_min=instant, utc_max=instant, max_delta_ms=1800000,
                clock_quality="pc_unverified"))
            self.store.save(device(event_id=RING+":"+str(index+1),
                fields={"ring": RING, "boot": 3, "seq": index+1, "uptime_ms": index*60000}))
        output = io.StringIO()
        self.assertEqual(export_timeline(self.store, output, "store-1", limit=1,
            window_start=instant, window_end=instant), 1)
        rows = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertFalse(rows[0]["truncated"])
        self.assertEqual(rows[1]["event"]["fields"]["seq"], 1100)
        self.assertEqual(rows[1]["event"]["alignment"]["interval_scope"], "collector_pc_clock")
        self.assertIsNone(rows[1]["interval_utc"])

    def test_many_matching_anchor_details_bounded_but_conflict_not_omitted(self):
        values = (anchor(anchor_id=str(index), utc_min="2026-10-25T00:00:00Z",
                         utc_max="2026-10-25T00:00:00Z") if index == 1100 else
                  anchor(anchor_id=str(index)) for index in range(1101))
        result = align_event(device(), values)
        self.assertEqual(result["alignment"]["reason"], "conflicting measured anchors")
        self.assertEqual(result["alignment"]["anchor_count"], 1101)
        self.assertEqual(len(result["alignment"]["anchor_ids"]), 64)
        self.assertTrue(result["alignment"]["anchor_details_truncated"])

    def test_sql_window_uses_exact_timedelta_rounding(self):
        self.store.save(device(fields={"ring": RING, "boot": 3, "seq": 10, "uptime_ms": 1001}))
        self.store.save_anchor(anchor(utc_max=NOW, drift_ppm=600))
        output = io.StringIO()
        boundary = "2026-10-24T00:00:00.001001Z"
        self.assertEqual(export_timeline(self.store, output, "store-1", limit=1,
                                        window_start=boundary, window_end=boundary), 1)

    def test_pc_clock_bracket_overlap_is_kept_without_certified_utc(self):
        self.store.save(device())
        self.store.save_anchor(anchor(clock_quality="pc_unverified"))
        output = io.StringIO()
        self.assertEqual(export_timeline(self.store, output, "store-1", window_start=NOW, window_end=NOW), 1)
        row = json.loads(output.getvalue().splitlines()[1])
        self.assertIsNone(row["interval_utc"])
        self.assertEqual(row["window_relation"], "pc_clock_overlap_unverified")


if __name__ == "__main__":
    unittest.main()
