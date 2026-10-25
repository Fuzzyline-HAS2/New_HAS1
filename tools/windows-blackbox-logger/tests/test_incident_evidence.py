"""Blackbox evidence summary: bounded, sequence-anchored, never a cause."""
from contextlib import closing
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from incident_evidence import DEVICE_FETCH, MAX_ROWS, summarize, wait_seconds
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


def gate(seq, observed, flags=0, wifi=3, **boot):
    """worker.queue.gates: queue depth flips 0/1 about once a second; flags are driver fault etc.; wifi 3 is connected."""
    return device(seq, observed, "Recovery", "worker.queue.gates", (seq % 2, flags, wifi, 0), **boot)


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
        # A power-on boot only re-reports the dump kept in flash: not a crash of this reboot.
        self.assertEqual(result["highlights"], ["재부팅 1번(리셋 원인 1 POWERON 전원 켜짐)", "와이파이 끊김 원인 202 AUTH_FAIL",
                                                "이전 크래시 덤프 1건(이번 재부팅 원인 아님)"])
        texts = [row[2] for row in full]
        self.assertEqual(texts[1], "모터·LED 출력 변경 30건")
        self.assertEqual(full[1][1], "부팅 1 · 켜진 뒤 0:01:02~0:01:31 (수신 12:03:30)")
        self.assertEqual(texts[2], "🔁 재부팅 (부팅 1 → 2) · 리셋 원인 1 POWERON 전원 켜짐")
        self.assertIn("리셋 원인 1 POWERON 전원 켜짐", texts[3])
        self.assertTrue(texts[4].startswith("이전 크래시 기록 · prior.core:mqtt_task"))
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

    def test_queue_gate_noise_collapses_and_keeps_the_reboot_explanation(self):
        # Generator BG (firmware 39): worker.queue.gates logged depth 0/1 every second; its 40 rows hid the
        # reboot, the Boot record, the dump and the Wi-Fi event, and "일부만 표시했습니다" explained nothing.
        at, boot2 = "2026-10-24T03:03:30.000000+00:00", dict(boot=2994054713, uptime=0)
        result = self.summarize_boots(
            "queue-gates.db",
            *[gate(seq, at) for seq in range(2, 152)],
            device(152, at, "Boot", "device.boot", (1, 39, 1, 0), **boot2),
            device(153, at, "Crash", "prior.core:mqtt_task", (1074326982, 70, 0, 65540), **boot2),
            device(154, at, "Crash", "prior.core.bt0", (1074326982, 1074327000, 1074327100, 1074327200), **boot2),
            device(155, at, "Crash", "prior.core.elf:abcdef123", (1, 123456, 0, 0), **boot2),
            device(156, at, "WifiEvent", "ap:ACA7F1C619E8", (113, 202, 1, 0), **boot2),
            *[gate(seq, at, **boot2) for seq in range(157, 187)])
        rows = [row for row in result["rows"] if row[0] == "장치"]
        texts = [row[2] for row in rows]
        self.assertLessEqual(len(result["rows"]), MAX_ROWS)
        self.assertFalse(result["truncated"])
        self.assertEqual(len(rows), 9)
        self.assertEqual(texts[1], "작업 큐 상태 변경 150건")
        self.assertEqual(rows[1][1], "부팅 1 · 켜진 뒤 0:01:02~0:03:31 (수신 12:03:30)")
        self.assertEqual(texts[2], "🔁 재부팅 (부팅 1 → 2) · 리셋 원인 1 POWERON 전원 켜짐")
        self.assertTrue(texts[3].startswith("부팅 · device.boot"))
        self.assertTrue(all(text.startswith("이전 크래시 기록 · prior.core") for text in texts[4:7]))
        self.assertIn("끊김 원인 202 AUTH_FAIL", texts[7])
        self.assertEqual(texts[8], "작업 큐 상태 변경 30건")
        self.assertFalse(any("worker.queue.gates" in text for text in texts))
        # The gate records say nothing about reboots, Wi-Fi or dumps: the chat one-liner is unchanged.
        self.assertEqual(result["highlights"], ["재부팅 1번(리셋 원인 1 POWERON 전원 켜짐)", "와이파이 끊김 원인 202 AUTH_FAIL",
                                                "이전 크래시 덤프 1건(이번 재부팅 원인 아님)"])

    def test_queue_gate_with_a_flag_set_stays_visible_as_its_own_row(self):
        # Driver fault (4), maintenance (8), scan pending (2) and mailbox (1) are informative; only flags 0 is noise.
        at = "2026-10-24T03:03:30.000000+00:00"
        result = self.summarize_boots(
            "queue-flags.db",
            *[gate(seq, at) for seq in range(2, 22)],
            gate(22, at, flags=4),
            *[gate(seq, at) for seq in range(23, 43)])
        texts = [row[2] for row in result["rows"] if row[0] == "장치"]
        self.assertEqual(len(texts), 4)
        self.assertEqual(texts[1], "작업 큐 상태 변경 20건")
        self.assertEqual(texts[2], "복구 동작 · worker.queue.gates · queue_depth 0 · flags 4 · wifi_status 3")
        self.assertEqual(texts[3], "작업 큐 상태 변경 20건")

    def test_queue_gate_runs_do_not_merge_with_output_runs_or_across_a_reboot(self):
        at, boot2 = "2026-10-24T03:03:30.000000+00:00", dict(boot=2994054713, uptime=0)
        result = self.summarize_boots(
            "queue-runs.db",
            device(2, at, "State", "motor.applied", (1, 4, 0, 0)),
            gate(3, at),
            gate(4, at),
            gate(5, at, **boot2))
        texts = [row[2] for row in result["rows"] if row[0] == "장치"]
        self.assertEqual(texts[1:], ["모터·LED 출력 변경 1건", "작업 큐 상태 변경 2건",
                                     "🔁 재부팅 (부팅 1 → 2)", "작업 큐 상태 변경 1건"])

    def test_queue_gate_flood_does_not_starve_the_key_fetch(self):
        # More routine gate records than DEVICE_FETCH before the reboot: the key query must not spend its cap on them.
        at, boot2 = "2026-10-24T03:03:30.000000+00:00", dict(boot=2994054713, uptime=0)
        flood = DEVICE_FETCH + 50
        result = self.summarize_boots(
            "queue-flood.db",
            *[gate(seq, at) for seq in range(2, 2 + flood)],
            device(2 + flood, at, "Boot", "device.boot", (1, 39, 1, 0), **boot2),
            device(3 + flood, at, "WifiEvent", "ap:ACA7F1C619E8", (113, 202, 1, 0), **boot2),
            gate(4 + flood, at, flags=4, **boot2))
        texts = [row[2] for row in result["rows"] if row[0] == "장치"]
        self.assertTrue(result["truncated"])
        self.assertIn(f"작업 큐 상태 변경 {DEVICE_FETCH}건", texts)
        self.assertIn("🔁 재부팅 (부팅 1 → 2) · 리셋 원인 1 POWERON 전원 켜짐", texts)
        self.assertTrue(any(text.startswith("부팅 · device.boot") for text in texts))
        self.assertTrue(any("끊김 원인 202 AUTH_FAIL" in text for text in texts))
        # A flagged gate record is key too: it is fetched even past the cap.
        self.assertTrue(any("worker.queue.gates" in text and "flags 4" in text for text in texts))
        self.assertEqual(result["highlights"][0], "재부팅 1번(리셋 원인 1 POWERON 전원 켜짐)")

    def test_boot_crash_wifi_and_reboot_rows_outrank_other_key_rows_at_the_limit(self):
        # Wi-Fi retries alone overflow the table: the rows that explain the reboot must not be cut with them.
        at, boot2 = "2026-10-24T03:03:30.000000+00:00", dict(boot=2994054713, uptime=0)
        result = self.summarize_boots(
            "key-tiers.db",
            *[device(seq, at, "WifiTry", "connect.begin") for seq in range(2, 62)],
            device(62, at, "Boot", "device.boot", (1, 39, 1, 0), **boot2),
            device(63, at, "Crash", "prior.core:mqtt_task", (1074326982, 70, 0, 65540), **boot2),
            device(64, at, "WifiEvent", "ap:ACA7F1C619E8", (113, 202, 1, 0), **boot2),
            *[device(seq, at, "WifiTry", "connect.begin", **boot2) for seq in range(65, 125)])
        texts = [row[2] for row in result["rows"]]
        self.assertTrue(result["truncated"])
        self.assertEqual(len(texts), MAX_ROWS)
        self.assertIn("🔁 재부팅 (부팅 1 → 2) · 리셋 원인 1 POWERON 전원 켜짐", texts)
        self.assertTrue(any(text.startswith("부팅 · device.boot") for text in texts))
        self.assertTrue(any(text.startswith("이전 크래시 기록 · prior.core") for text in texts))
        self.assertTrue(any("끊김 원인 202 AUTH_FAIL" in text for text in texts))
        # The rest of the room goes to the other key rows, still the first and the last of them.
        retries = [row for row in result["rows"] if "connect.begin" in row[2]]
        self.assertEqual(len(retries), MAX_ROWS - 4 - 1)  # minus the four rows above and the Wi-Fi summary
        self.assertTrue(result["rows"][0][2].startswith("와이파이 요약"))
        self.assertIn("connect.begin", texts[-1])

    def test_queue_gate_runs_split_where_the_wifi_status_changes(self):
        # Flags 0 is routine, but wifi_status (values[2]) 5/6 is the link dropping: it must not fold into the connected run.
        at = "2026-10-24T03:03:30.000000+00:00"
        result = self.summarize_boots(
            "queue-wifi-status.db",
            *[gate(seq, at) for seq in range(2, 7)],
            *[gate(seq, at, wifi=6) for seq in range(7, 11)],
            *[gate(seq, at) for seq in range(11, 14)],
            *[gate(seq, at, wifi=5) for seq in range(14, 16)])
        texts = [row[2] for row in result["rows"] if row[0] == "장치"]
        self.assertEqual(texts[1:], ["작업 큐 상태 변경 5건", "작업 큐 상태 변경 4건 · 와이파이 상태 6",
                                     "작업 큐 상태 변경 3건", "작업 큐 상태 변경 2건 · 와이파이 상태 5"])

    def test_reboot_marker_and_boot_row_outrank_wifi_disconnects_at_the_limit(self):
        # A Wi-Fi disconnect storm on both sides of a reset: the marker and the Boot row are what the
        # chat highlight ("재부팅 1번") points at, so they must survive the first-and-last halves of the Wi-Fi tier.
        at, boot2 = "2026-10-24T03:03:30.000000+00:00", dict(boot=2994054713, uptime=0)
        result = self.summarize_boots(
            "wifi-storm.db",
            *[device(seq, at, "WifiEvent", "ap:ACA7F1C619E8", (113, 201, 1, 0)) for seq in range(2, 62)],
            device(62, at, "Boot", "device.boot", (3, 39, 1, 0), **boot2),
            *[device(seq, at, "WifiEvent", "ap:ACA7F1C619E8", (113, 201, 1, 0), **boot2) for seq in range(63, 123)])
        texts = [row[2] for row in result["rows"]]
        self.assertTrue(result["truncated"])
        self.assertLessEqual(len(texts), MAX_ROWS)
        self.assertIn("🔁 재부팅 (부팅 1 → 2) · 리셋 원인 3 SW 소프트웨어 재시작", texts)
        self.assertTrue(any(text.startswith("부팅 · device.boot") for text in texts))
        self.assertEqual(result["highlights"][0], "재부팅 1번(리셋 원인 3 SW 소프트웨어 재시작)")
        # The Wi-Fi tier is what gets halved: its first and its last rows stay.
        wifi = [row for row in result["rows"] if "NO_AP_FOUND" in row[2]]
        self.assertEqual(len(wifi), MAX_ROWS - 2)  # everything but the marker and the Boot row; the Wi-Fi summary goes first
        self.assertIn("부팅 1 ·", wifi[0][1])
        self.assertIn("부팅 2 ·", wifi[-1][1])

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

    def summarize_boots(self, name, *records):
        path = Path(self.temp.name) / name
        store = TelemetryStore(path)
        store.save(device(1, "2026-10-24T02:59:00.000000+00:00", "WifiSummary", "wifi", (1, -72, 0, 0)))
        for record in records:
            store.save(record)
        store.close()
        with closing(_open_telemetry(path)) as raw:
            return summarize(raw, INCIDENT, CLOSED)

    def test_power_on_boot_reports_the_kept_dump_as_older_not_a_crash(self):
        # Generator BG (firmware 39), power-off/on test: "크래시 기록 4건" came from the dump kept in flash
        # (its three records plus the RTC trail marker).
        boot = dict(boot=2994054713, uptime=0)
        at = "2026-10-24T03:03:35.000000+00:00"
        result = self.summarize_boots(
            "power-on.db",
            device(2, at, "Boot", "device.boot", (1, 39, 1, 0), **boot),
            device(3, at, "Crash", "prior.rtc.coverage", (0, 0, 0, 0), **boot),
            device(4, at, "Crash", "prior.core:mqtt_task", (1074326982, 70, 0, 65540), **boot),
            device(5, at, "Crash", "prior.core.bt0", (1074326982, 1074327000, 1074327100, 1074327200), **boot),
            device(6, at, "Crash", "prior.core.elf:abcdef123", (1, 123456, 0, 0), **boot))
        # One dump, not its four records, and no plain crash claim.
        self.assertEqual(result["highlights"], ["재부팅 1번(리셋 원인 1 POWERON 전원 켜짐)", "이전 크래시 덤프 1건(이번 재부팅 원인 아님)"])
        dump_rows = [row[2] for row in result["rows"] if "prior.core" in row[2]]
        self.assertEqual(len(dump_rows), 3)
        self.assertTrue(all(text.startswith("이전 크래시 기록 · prior.core") for text in dump_rows))
        # The RTC trail is the previous boot's, not a dump: its row keeps its kind.
        self.assertIn("크래시 · prior.rtc.coverage", [row[2][:len("크래시 · prior.rtc.coverage")] for row in result["rows"]])

    def test_panic_boot_with_a_dump_is_still_a_crash(self):
        boot = dict(boot=2994054713, uptime=0)
        at = "2026-10-24T03:03:35.000000+00:00"
        result = self.summarize_boots(
            "panic.db",
            device(2, at, "Boot", "device.boot", (4, 39, 1, 0), **boot),
            device(3, at, "Crash", "prior.core:mqtt_task", (1074326982, 70, 0, 262660), **boot),
            device(4, at, "Crash", "prior.core.bt0", (1074326982, 1074327000, 1074327100, 1074327200), **boot),
            device(5, at, "Crash", "prior.core.elf:abcdef123", (1, 123456, 0, 0), **boot))
        self.assertEqual(result["highlights"], ["재부팅 1번(리셋 원인 4 PANIC 크래시)", "크래시 덤프 1건"])
        crash_rows = [row[2] for row in result["rows"] if "prior.core" in row[2]]
        self.assertTrue(all(text.startswith("크래시 · prior.core") for text in crash_rows))

    def test_dump_header_flags_decide_over_the_boot_record(self):
        at = "2026-10-24T03:03:35.000000+00:00"
        # A brownout writes no dump: one logged after it is the kept one, whatever the boot says.
        brownout = self.summarize_boots(
            "brownout.db",
            device(2, at, "Boot", "device.boot", (9, 39, 1, 0), boot=1, uptime=0),
            device(3, at, "Crash", "prior.core:mqtt_task", (1074326982, 70, 0, (9 << 16) | 4), boot=1, uptime=0))
        self.assertEqual(brownout["highlights"], ["재부팅 1번(리셋 원인 9 BROWNOUT 전압 저하)", "이전 크래시 덤프 1건(이번 재부팅 원인 아님)"])
        # A fresh header is this boot's crash even when its Boot record fell outside the window.
        fresh = self.summarize_boots(
            "fresh.db", device(2, at, "Crash", "prior.core:mqtt_task", (1074326982, 70, 0, 262660), boot=1, uptime=0))
        self.assertEqual(fresh["highlights"], ["재부팅 1번", "크래시 덤프 1건"])
        # Firmware from before the flags: the boot's reset reason decides.
        legacy = self.summarize_boots(
            "legacy.db",
            device(2, at, "Boot", "device.boot", (4, 39, 1, 0), boot=1, uptime=0),
            device(3, at, "Crash", "prior.core:mqtt_task", (1074326982, 70, 0, 4), boot=1, uptime=0),
            device(4, at, "Boot", "device.boot", (1, 39, 1, 0), boot=2, uptime=0),
            device(5, at, "Crash", "prior.core:mqtt_task", (1074326982, 70, 0, 4), boot=2, uptime=0),
            device(6, at, "Boot", "device.boot", (9, 39, 1, 0), boot=3, uptime=0),  # a brownout writes no dump
            device(7, at, "Crash", "prior.core:mqtt_task", (1074326982, 70, 0, 4), boot=3, uptime=0))
        self.assertEqual(legacy["highlights"][1:], ["크래시 덤프 1건", "이전 크래시 덤프 2건(이번 재부팅 원인 아님)"])
        # A glove's link-loss record is Crash-typed but no dump: neither counted nor relabelled.
        glove = self.summarize_boots(
            "glove.db",
            device(2, at, "Boot", "device.boot", (3, 39, 1, 0), boot=1, uptime=0),
            device(3, at, "Crash", "prior.link_loss", (1, 2, -70, 0), boot=1, uptime=0))
        self.assertEqual(glove["highlights"], ["재부팅 1번(리셋 원인 3 SW 소프트웨어 재시작)"])
        self.assertTrue(any(row[2].startswith("크래시 · prior.link_loss") for row in glove["rows"]))

    def test_each_boot_is_judged_by_its_own_reset_reason(self):
        power_on, watchdog = dict(boot=2994054713, uptime=0), dict(boot=1111111111, uptime=0)
        at = "2026-10-24T03:03:35.000000+00:00"
        result = self.summarize_boots(
            "mixed.db",
            device(2, at, "Boot", "device.boot", (1, 39, 1, 0), **power_on),
            device(3, at, "Crash", "prior.core:mqtt_task", (1074326982, 70, 0, 65540), **power_on),
            device(4, at, "Crash", "prior.core.elf:abcdef123", (1, 123456, 0, 0), **power_on),
            device(5, at, "Boot", "device.boot", (6, 39, 1, 0), **watchdog),
            device(6, at, "Crash", "prior.core:loopTask", (1074326982, 70, 0, 393732), **watchdog),
            device(7, at, "Crash", "prior.core.elf:fedcba987", (1, 654321, 0, 0), **watchdog))
        self.assertEqual(result["highlights"], ["재부팅 2번(리셋 원인 1 POWERON 전원 켜짐)", "크래시 덤프 1건",
                                                "이전 크래시 덤프 1건(이번 재부팅 원인 아님)"])

    def test_crash_records_without_a_crash_reset_are_not_claimed_as_crashes(self):
        # Soft restart: only the RTC trail of the previous boot. Unknown reason: no Boot record in the window.
        at = "2026-10-24T03:03:35.000000+00:00"
        restart = self.summarize_boots(
            "restart.db",
            device(2, at, "Boot", "device.boot", (3, 39, 1, 0), boot=2994054713, uptime=0),
            device(3, at, "Crash", "prior.rtc.event", (5, 1, -1, 0), boot=2994054713, uptime=0))
        self.assertEqual(restart["highlights"], ["재부팅 1번(리셋 원인 3 SW 소프트웨어 재시작)"])
        unknown = self.summarize_boots(
            "unknown.db", device(2, at, "Crash", "prior.core:mqtt_task", (1074326982, 70, 0, 65540), boot=2994054713, uptime=0))
        self.assertEqual(unknown["highlights"], ["재부팅 1번", "이전 크래시 덤프 1건(이번 재부팅 원인 아님)"])

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
