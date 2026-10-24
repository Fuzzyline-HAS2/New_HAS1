"""Blackbox evidence summary: bounded, sequence-anchored, never a cause."""
from contextlib import closing
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from incident_evidence import summarize, wait_seconds
from incidents import _open_telemetry
from telemetry import Event, TelemetryStore

MAC = "AABBCCDDEEFF"
OPENED = "2026-10-24T03:00:00.000000+00:00"
CLOSED = "2026-10-24T03:03:20.000000+00:00"
INCIDENT = {"incident_id": "incident-1", "site_id": "store-1", "source_id": "server-1", "mac": MAC,
            "opened_utc": OPENED, "updated_utc": CLOSED}


def device(seq, observed, kind="State", text="", values=(0, 0, 0, 0), boot=485740653, uptime=None):
    return Event(site_id="store-1", source="device", source_id=MAC, event_id=f"r1:{seq}", observed_utc=observed,
                 occurred_utc=None, clock_quality="uptime_only", mac=MAC, device_id="iotglove", event_type=kind,
                 fields={"ring": "r1", "seq": seq, "boot": boot, "uptime_ms": 60000 + seq * 1000 if uptime is None else uptime,
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
        self.assertEqual(device_rows[0][1], "부팅 1 · 켜진 뒤 0:01:02 (수신 11:59:00)")
        self.assertIn("끊김 원인 200 BEACON_TIMEOUT", device_rows[5][2])
        self.assertIn("AP 112233445566", device_rows[5][2])
        self.assertIn("끊김 시간 3분 15초 · 시도 7", device_rows[6][2])
        self.assertFalse(result["device_pending"])
        self.assertFalse(result["truncated"])
        self.assertEqual(result["highlights"], ["와이파이 끊김 원인 200 BEACON_TIMEOUT"])
        self.assertTrue(summarize(self.raw, INCIDENT, CLOSED, limit=3)["truncated"])

    def test_waits_for_device_backlog_then_gives_up(self):
        self.assertEqual(wait_seconds(self.raw, INCIDENT, "2026-10-24T03:04:20.000000+00:00"), 71)
        self.assertEqual(wait_seconds(self.raw, INCIDENT, "2026-10-24T03:06:00.000000+00:00"), 0)
        silent = dict(INCIDENT, mac="112233445566")
        self.assertEqual(wait_seconds(self.raw, silent, "2026-10-24T03:04:20.000000+00:00"), 61)
        self.assertEqual(wait_seconds(self.raw, silent, "2026-10-24T09:03:20.000000+00:00"), 0)
        self.assertEqual(wait_seconds(self.raw, dict(INCIDENT, mac=None), CLOSED), 0)
        result = summarize(self.raw, silent, "2026-10-24T09:03:20.000000+00:00")
        self.assertTrue(result["device_pending"])
        self.assertEqual(result["highlights"], ["장치 기록 아직 없음"])
        self.assertFalse(any(row[0] == "장치" for row in result["rows"]))

    def test_backlog_arriving_before_server_recovery_still_counts(self):
        # Upload at 03:03:30 precedes a recovery the server only declares at 03:04:00.
        late_close = dict(INCIDENT, updated_utc="2026-10-24T03:04:00.000000+00:00")
        self.assertEqual(wait_seconds(self.raw, late_close, "2026-10-24T03:05:00.000000+00:00"), 61)
        self.assertEqual(wait_seconds(self.raw, late_close, "2026-10-24T03:06:00.000000+00:00"), 0)
        self.assertFalse(summarize(self.raw, late_close, "2026-10-24T03:06:00.000000+00:00")["device_pending"])

    def test_output_spam_collapses_and_reboot_crash_wifi_survive_the_limit(self):
        # Game 1654: haptic/LED output changes fill the backlog, then the glove reboots.
        path = Path(self.temp.name) / "reboot.db"
        store = TelemetryStore(path)
        store.save(device(1, "2026-10-24T02:59:00.000000+00:00", "WifiSummary", "wifi", (1, -72, 0, 0)))
        for seq in range(2, 32):
            store.save(device(seq, "2026-10-24T03:03:30.000000+00:00", "State", ("motor.applied", "output.applied")[seq % 2], (1, 4, 0, 0)))
        new_boot = dict(boot=2994054713, uptime=0)
        store.save(device(32, "2026-10-24T03:03:35.000000+00:00", "Boot", "device.boot", (1, 22, 1, 0), **new_boot))
        store.save(device(33, "2026-10-24T03:03:35.000000+00:00", "Crash", "prior.core:mqtt_task", (1074326982, 70, 0, 65540), **new_boot))
        store.save(device(34, "2026-10-24T03:03:35.000000+00:00", "WifiEvent", "ap:ACA7F1C619E8", (113, 202, 1, 0), **new_boot))
        store.save(device(35, "2026-10-24T03:03:35.000000+00:00", "State", "motor.applied", (0, 0, 0, 0), **new_boot))
        store.close()
        with closing(_open_telemetry(path)) as raw:
            result = summarize(raw, INCIDENT, CLOSED)
            full = [row for row in result["rows"] if row[0] == "장치"]
            tight = summarize(raw, INCIDENT, CLOSED, limit=5)  # exactly the five key rows
        self.assertEqual(result["highlights"], ["재부팅 1번(리셋 원인 1 POWERON 전원 켜짐)", "와이파이 끊김 원인 202 AUTH_FAIL", "크래시 기록 1건"])
        texts = [row[2] for row in full]
        self.assertEqual(texts[1], "모터·LED 출력 변경 30건")
        self.assertEqual(full[1][1], "부팅 1 · 켜진 뒤 0:01:02~0:01:31 (수신 12:03:30)")
        self.assertEqual(texts[2], "🔁 재부팅 (부팅 1 → 2) · 리셋 원인 1 POWERON 전원 켜짐")
        self.assertIn("리셋 원인 1 POWERON 전원 켜짐", texts[3])
        self.assertIn("끊김 원인 202 AUTH_FAIL", texts[5])
        self.assertEqual(texts[6], "모터·LED 출력 변경 1건")
        self.assertTrue(all(row[1].startswith("부팅 2 ") for row in full[2:]))
        # Over the limit, the routine rows go first and the explanation stays.
        kept = [row[2] for row in tight["rows"]]
        self.assertTrue(tight["truncated"])
        self.assertEqual(len(kept), 5)
        self.assertFalse(any("출력 변경" in text for text in kept))
        self.assertTrue(any("재부팅" in text for text in kept) and any("AUTH_FAIL" in text for text in kept))

    def test_same_second_runs_show_one_time_and_unavailable_values_are_hidden(self):
        # Game 1656: "0:06:53~0:06:53" and "부팅 · 22 · 값 1 -2147483648 -2147483648 -2147483648".
        path = Path(self.temp.name) / "polish.db"
        store = TelemetryStore(path)
        store.save(device(1, "2026-10-24T02:59:00.000000+00:00", "WifiSummary", "wifi", (1, -72, 0, 0)))
        for seq in (2, 3):
            store.save(device(seq, "2026-10-24T03:03:30.000000+00:00", "State", "motor.applied", (1, 4, 0, 0), uptime=413000))
        store.save(device(4, "2026-10-24T03:03:30.000000+00:00", "Boot", "22", (1, -2147483648, -2147483648, -2147483648)))
        store.save(device(5, "2026-10-24T03:03:30.000000+00:00", "Boot", "blank", (-2147483648,) * 4))
        store.close()
        with closing(_open_telemetry(path)) as raw:
            result = summarize(raw, INCIDENT, CLOSED)
            rows = [row for row in result["rows"] if row[0] == "장치"]
        # Game 1658: the glove logged no reboot, Wi-Fi drop or crash; say so rather than nothing.
        self.assertEqual(result["highlights"], ["재부팅·와이파이 끊김·크래시 기록 없음"])
        self.assertEqual(rows[1], ["장치", "부팅 1 · 켜진 뒤 0:06:53 (수신 12:03:30)", "모터·LED 출력 변경 2건"])
        self.assertEqual(rows[2][2], "부팅 · 22 · 값 1")
        self.assertEqual(rows[3][2], "부팅 · blank")

    def test_key_records_survive_the_fetch_cap_and_keep_the_outage_end(self):
        path = Path(self.temp.name) / "spam.db"
        store = TelemetryStore(path)
        store.save(device(1, "2026-10-24T02:59:00.000000+00:00", "WifiSummary", "wifi", (1, -72, 0, 0)))
        for seq in range(2, 252):  # more output changes than DEVICE_FETCH reads
            store.save(device(seq, "2026-10-24T03:03:30.000000+00:00", "State", "motor.applied", (1, 4, 0, 0)))
        for seq in range(252, 262):  # repeated reconnect attempts, then the recovery that ends it
            store.save(device(seq, "2026-10-24T03:03:35.000000+00:00", "WifiTry", "connect.begin"))
        store.save(device(262, "2026-10-24T03:03:35.000000+00:00", "Recovery", "wifi.offline.recovery", (1, 13000, 10, 0)))
        store.close()
        with closing(_open_telemetry(path)) as raw:
            full = summarize(raw, INCIDENT, CLOSED)
            tight = summarize(raw, INCIDENT, CLOSED, limit=6)
        texts = [row[2] for row in full["rows"]]
        self.assertTrue(full["truncated"])
        self.assertIn("모터·LED 출력 변경 200건", texts)
        self.assertEqual(sum("connect.begin" in text for text in texts), 10)
        self.assertIn("끊김 시간 13.0초", texts[-1])
        kept = [row[2] for row in tight["rows"]]
        # Twelve key rows for six slots: the first three and the last three, in order.
        self.assertEqual(len(kept), 6)
        self.assertTrue(kept[0].startswith("와이파이 요약"))
        self.assertIn("복구 동작", kept[-1])

    def test_reset_reason_comes_from_the_evidence_boot_record_too(self):
        # Game 1659 G2P7: the table was built before device.boot arrived; evidence.* already had the reason.
        path = Path(self.temp.name) / "evidence-boot.db"
        store = TelemetryStore(path)
        store.save(device(1, "2026-10-24T02:59:00.000000+00:00", "WifiSummary", "wifi", (1, -78, 0, 0)))
        new_boot = dict(boot=2994054713, uptime=0)
        store.save(device(2, "2026-10-24T03:03:30.000000+00:00", "Logger", "rtc.replay.end", (0, 0, 1, 0), **new_boot))
        store.save(device(3, "2026-10-24T03:03:30.000000+00:00", "Boot", "evidence.20261024.2", (1, 2, 0, 0), **new_boot))
        store.save(device(4, "2026-10-24T03:03:30.000000+00:00", "Boot", "23", (1, -2147483648, -2147483648, -2147483648), **new_boot))
        store.close()
        with closing(_open_telemetry(path)) as raw:
            result = summarize(raw, INCIDENT, CLOSED)
        self.assertIn("🔁 재부팅 (부팅 1 → 2) · 리셋 원인 1 POWERON 전원 켜짐", [row[2] for row in result["rows"]])
        self.assertEqual(result["highlights"], ["재부팅 1번(리셋 원인 1 POWERON 전원 켜짐)"])

    def test_unmapped_wifi_reasons_still_summarize(self):
        # ESP-IDF has many disconnect codes beyond WIFI_REASONS; they must not break the table.
        path = Path(self.temp.name) / "unmapped.db"
        store = TelemetryStore(path)
        store.save(device(1, "2026-10-24T02:59:00.000000+00:00", "WifiSummary", "wifi", (1, -72, 0, 0)))
        store.save(device(2, "2026-10-24T03:03:30.000000+00:00", "WifiEvent", "ap:112233445566", (21, 14, 6, 0)))
        store.save(device(3, "2026-10-24T03:03:30.000000+00:00", "WifiEvent", "ap:112233445566", (21, 202, 6, 0)))
        store.save(device(4, "2026-10-24T03:03:30.000000+00:00", "WifiEvent", "ap:112233445566", (21, 9, 6, 0)))
        store.close()
        with closing(_open_telemetry(path)) as raw:
            result = summarize(raw, INCIDENT, CLOSED)
        self.assertEqual(result["highlights"], ["와이파이 끊김 원인 14, 202 AUTH_FAIL, 9"])
        self.assertIn("끊김 원인 14", result["rows"][-3][2])

    def test_highlight_failure_never_costs_the_table(self):
        with patch("incident_evidence._highlights", side_effect=TypeError("boom")):
            result = summarize(self.raw, INCIDENT, CLOSED)
        self.assertEqual(result["highlights"], [])
        self.assertTrue(result["rows"])

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
