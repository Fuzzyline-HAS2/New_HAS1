"""Blackbox evidence summary: bounded, sequence-anchored, never a cause."""
from contextlib import closing
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from incident_evidence import summarize, wait_seconds
from incidents import _open_telemetry
from telemetry import Event, TelemetryStore

MAC = "AABBCCDDEEFF"
OPENED = "2026-10-24T03:00:00.000000+00:00"
CLOSED = "2026-10-24T03:03:20.000000+00:00"
INCIDENT = {"incident_id": "incident-1", "site_id": "store-1", "source_id": "server-1", "mac": MAC,
            "opened_utc": OPENED, "updated_utc": CLOSED}


def device(seq, observed, kind="State", text="", values=(0, 0, 0, 0)):
    return Event(site_id="store-1", source="device", source_id=MAC, event_id=f"r1:{seq}", observed_utc=observed,
                 occurred_utc=None, clock_quality="uptime_only", mac=MAC, device_id="iotglove", event_type=kind,
                 fields={"ring": "r1", "seq": seq, "boot": 12, "uptime_ms": 60000 + seq * 1000,
                         "values": list(values), "text": text})


def server(kind, when, **fields):
    return Event(site_id="store-1", source="game_server", source_id="server-1", event_id=kind + when,
                 observed_utc=when, occurred_utc=when, mac=MAC, device_id="glove-1", event_type=kind,
                 fields=dict(incident_id="incident-1", **fields))


def omada(kind, when, name):
    return Event(site_id="store-1", source="omada", source_id="ap", event_id=name, observed_utc=when,
                 occurred_utc=when, mac=MAC, event_type=kind)


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        path = Path(self.temp.name) / "telemetry.db"
        store = TelemetryStore(path)
        for seq in range(1, 7):
            store.save(device(seq, "2026-10-24T02:59:00.000000+00:00"))
        # Backlog uploaded after the device reconnected: what happened during the outage.
        store.save(device(7, "2026-10-24T03:03:30.000000+00:00", "WifiEvent", "ap:112233445566", (21, 200, 6, 0)))
        store.save(device(8, "2026-10-24T03:03:30.000000+00:00", "Recovery", "wifi.offline.recovery", (1, 195000, 7, 0)))
        store.save(server("device_offline", OPENED, reason="timeout", gap_ms=12500))
        store.save(server("device_recovered", CLOSED, reason="reconnected"))
        store.save(server("game_monitor_snapshot", OPENED))
        store.save(omada("disconnected", "2026-10-24T02:59:58.000000+00:00", "left"))
        store.save(omada("connected", "2026-10-24T03:03:10.000000+00:00", "joined"))
        store.save(omada("unknown", "2026-10-24T03:01:00.000000+00:00", "traffic"))
        store.close()
        self.raw = _open_telemetry(path)

    def tearDown(self):
        self.raw.close()
        self.temp.cleanup()

    def test_summary_orders_sources_and_decodes_device_values(self):
        result = summarize(self.raw, INCIDENT, "2026-10-24T03:10:00.000000+00:00")
        rows = result["rows"]
        self.assertEqual([row[0] for row in rows[:4]], ["Omada", "게임 서버", "Omada", "게임 서버"])
        self.assertEqual(rows[1], ["게임 서버", "12:00:00", "서버가 끊김 감지 · timeout · 무응답 12.5초"])
        self.assertNotIn("traffic", str(rows))
        device_rows = [row for row in rows if row[0] == "장치"]
        # Five records before the last pre-outage report, then everything logged after it.
        self.assertEqual(len(device_rows), 7)
        self.assertEqual(device_rows[0][1], "부팅 12 · 켜진 뒤 0:01:02 (수신 11:59:00)")
        self.assertIn("끊김 원인 200 BEACON_TIMEOUT", device_rows[5][2])
        self.assertIn("AP 112233445566", device_rows[5][2])
        self.assertIn("끊김 시간 3분 15초 · 시도 7", device_rows[6][2])
        self.assertFalse(result["device_pending"])
        self.assertFalse(result["truncated"])
        self.assertTrue(summarize(self.raw, INCIDENT, CLOSED, limit=3)["truncated"])

    def test_waits_for_device_backlog_then_gives_up(self):
        self.assertEqual(wait_seconds(self.raw, INCIDENT, "2026-10-24T03:04:20.000000+00:00"), 71)
        self.assertEqual(wait_seconds(self.raw, INCIDENT, "2026-10-24T03:06:00.000000+00:00"), 0)
        silent = dict(INCIDENT, mac="112233445566")
        self.assertEqual(wait_seconds(self.raw, silent, "2026-10-24T03:04:20.000000+00:00"), 601)
        self.assertEqual(wait_seconds(self.raw, silent, "2026-10-24T09:03:20.000000+00:00"), 0)
        self.assertEqual(wait_seconds(self.raw, dict(INCIDENT, mac=None), CLOSED), 0)
        result = summarize(self.raw, silent, "2026-10-24T09:03:20.000000+00:00")
        self.assertTrue(result["device_pending"])
        self.assertFalse(any(row[0] == "장치" for row in result["rows"]))

    def test_backlog_arriving_before_server_recovery_still_counts(self):
        # Upload at 03:03:30 precedes a recovery the server only declares at 03:04:00.
        late_close = dict(INCIDENT, updated_utc="2026-10-24T03:04:00.000000+00:00")
        self.assertEqual(wait_seconds(self.raw, late_close, "2026-10-24T03:05:00.000000+00:00"), 61)
        self.assertEqual(wait_seconds(self.raw, late_close, "2026-10-24T03:06:00.000000+00:00"), 0)
        self.assertFalse(summarize(self.raw, late_close, "2026-10-24T03:06:00.000000+00:00")["device_pending"])

    def test_flapping_omada_keeps_start_and_end_and_reports_the_cut(self):
        path = Path(self.temp.name) / "flap.db"
        store = TelemetryStore(path)
        for minute in range(3):
            for second in range(0, 60, 5):
                kind = "connected" if second % 10 else "disconnected"
                store.save(omada(kind, f"2026-10-24T03:0{minute}:{second:02d}.000000+00:00", f"flap{minute}{second}"))
        store.close()
        with closing(_open_telemetry(path)) as raw:
            result = summarize(raw, dict(INCIDENT, mac=MAC), CLOSED)
        times = [row[1] for row in result["rows"] if row[0] == "Omada"]
        self.assertEqual(len(times), 10)
        self.assertEqual((times[0], times[-1]), ("12:00:00", "12:02:55"))
        self.assertTrue(result["truncated"])


if __name__ == "__main__":
    unittest.main()
