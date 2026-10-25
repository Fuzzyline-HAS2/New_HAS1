"""Generator silence and crash-reboot rule: decisions, incident flow and cards."""
from datetime import timedelta
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from contextlib import closing
from unittest.mock import patch

from generator_watch import GeneratorWatch, replay
from incident_evidence import summarize
from incidents import IncidentRuntime, IncidentStore, _open_telemetry, validate_generators
from notifications import _event_properties, _send_slack_session, slack_event_reply, slack_message, slack_session_card
from telemetry import Event, TelemetryStore, parse_utc

START = parse_utc("2026-10-24T23:00:00+00:00")
GENERATORS = {"BG": "A4F00FD90BCC", "LG": "A4F00FD906BC", "SG": "A4F00FD90A74", "TG": "B03FD32DFEE0", "UG": "A4F00FD90930"}
GLOVE = "A4F00FD90B34"


def at(seconds):
    return START + timedelta(seconds=seconds)


def stamp(seconds):
    return at(seconds).isoformat(timespec="microseconds")


def traffic(watch, start, end, *, silent=(), step=1):
    """Every device requests each step seconds, except the named generators."""
    actions = []
    for second in range(start, end + 1, step):
        actions += watch.request(GLOVE, at(second))
        for name, mac in GENERATORS.items():
            if name not in silent:
                actions += watch.request(mac, at(second))
    return actions


class WatchTests(unittest.TestCase):
    def setUp(self):
        self.watch = GeneratorWatch(GENERATORS)
        self.watch.server("boot-1", at(0))

    def test_silence_needs_a_game_and_twenty_seconds(self):
        self.assertEqual(traffic(self.watch, 0, 40, silent=("BG",)), [])  # no game: never reported
        self.watch.game(True, "game-1", at(41))
        quiet = traffic(self.watch, 42, 60, silent=("BG",))
        self.assertEqual(quiet, [])  # counted from the game start, not from its last request at 0s
        opened = traffic(self.watch, 61, 61, silent=("BG",))
        self.assertEqual([(item["type"], item["names"], item["gap_ms"]) for item in opened], [("open", ["BG"], 20000)])
        self.assertEqual(traffic(self.watch, 62, 70, silent=("BG",)), [])  # reported once
        back = self.watch.request(GENERATORS["BG"], at(71))
        self.assertEqual([(item["type"], item["key"]) for item in back], [("recover", opened[0]["key"])])

    def test_server_wide_pause_is_not_a_generator_silence(self):
        self.watch.game(True, "game-1", at(0))
        traffic(self.watch, 0, 10)
        # Nothing at all for 32s (server stall), then everything resumes.
        self.assertEqual(traffic(self.watch, 42, 60), [])
        # A generator that stays quiet afterwards is still caught, 20s after its last request (60s).
        self.assertEqual(traffic(self.watch, 61, 79, silent=("UG",)), [])
        self.assertEqual([item["names"] for item in traffic(self.watch, 80, 80, silent=("UG",))], [["UG"]])

    def test_generators_dropping_together_are_one_group(self):
        self.watch.game(True, "game-1", at(0))
        traffic(self.watch, 0, 5)
        traffic(self.watch, 6, 7, silent=("BG", "LG"))
        actions = traffic(self.watch, 8, 30, silent=("BG", "LG", "SG", "TG", "UG"))
        self.assertEqual([(item["type"], item["names"]) for item in actions], [("open", ["BG", "LG", "SG", "TG", "UG"])])
        self.assertEqual(actions[0]["since"], at(5))
        # The group recovers once every member is back.
        self.assertEqual(self.watch.request(GENERATORS["BG"], at(31)), [])
        partial = traffic(self.watch, 32, 32, silent=("UG",))
        self.assertEqual(partial, [])
        self.assertEqual([item["type"] for item in self.watch.request(GENERATORS["UG"], at(33))], ["recover"])

    def test_two_silent_generators_are_reported_separately(self):
        self.watch.game(True, "game-1", at(0))
        traffic(self.watch, 0, 2)
        actions = traffic(self.watch, 3, 30, silent=("BG", "SG"))
        self.assertEqual(sorted(item["names"][0] for item in actions), ["BG", "SG"])

    def test_game_end_and_server_restart_end_open_silences(self):
        self.watch.game(True, "game-1", at(0))
        opened = traffic(self.watch, 0, 21, silent=("BG",))
        self.assertEqual(len(opened), 1)
        self.assertEqual(self.watch.game(False, None, at(22)), [{"type": "end", "key": opened[0]["key"], "reason": "game_ended"}])
        self.watch.game(True, "game-2", at(23))
        opened = traffic(self.watch, 23, 44, silent=("BG",))
        self.assertEqual(self.watch.server("boot-2", at(45)), [{"type": "end", "key": opened[0]["key"], "reason": "server_restarted"}])
        # The new server boot starts measurement again.
        self.assertEqual(traffic(self.watch, 46, 64, silent=("BG",)), [])

    def test_restored_incident_survives_its_game_and_recovers(self):
        self.watch.restore("incident-x", [GENERATORS["LG"]], at(0), "game-1")
        self.assertEqual(self.watch.game(True, "game-1", at(1)), [])
        self.assertEqual(self.watch.request(GENERATORS["LG"], at(2))[0], {"type": "recover", "key": "incident-x", "at": at(2)})
        self.watch.restore("incident-y", [GENERATORS["LG"]], at(3), "game-1")
        self.assertEqual(self.watch.game(True, "game-2", at(4)), [{"type": "end", "key": "incident-y", "reason": "game_ended"}])

    def test_member_that_came_back_is_watched_again(self):
        self.watch.game(True, "game-1", at(0))
        traffic(self.watch, 0, 3)
        group = traffic(self.watch, 4, 24, silent=tuple(GENERATORS))
        self.assertEqual(len(group), 1)
        traffic(self.watch, 25, 26, silent=("UG",))  # four are back, UG is still out
        actions = traffic(self.watch, 27, 47, silent=("UG", "BG"))
        self.assertEqual([(item["type"], item["names"], item["last_seen"]) for item in actions], [("open", ["BG"], at(26))])

    def test_late_request_from_before_the_outage_is_not_a_recovery(self):
        self.watch.game(True, "game-1", at(0))
        traffic(self.watch, 0, 2)
        opened = traffic(self.watch, 3, 22, silent=("BG",))
        self.assertEqual([item["since"] for item in opened], [at(2)])
        self.assertEqual(self.watch.request(GENERATORS["BG"], at(1)), [])
        self.assertEqual(self.watch.request(GENERATORS["BG"], at(10)), [])  # stamped before it was reported
        self.assertEqual(self.watch.request(GENERATORS["BG"], at(23))[0]["type"], "recover")

    def test_short_silence_setting_does_not_group_healthy_generators(self):
        watch = GeneratorWatch(GENERATORS, silence_seconds=10)
        watch.server("boot-1", at(0))
        watch.game(True, "game-1", at(0))
        traffic(watch, 0, 2)
        self.assertEqual([item["names"] for item in traffic(watch, 3, 12, silent=("BG",))], [["BG"]])

    def test_late_line_from_previous_boot_is_not_a_restart(self):
        self.watch.game(True, "game-1", at(0))
        self.watch.server("boot-2", at(10))
        opened = traffic(self.watch, 10, 31, silent=("BG",))
        self.assertEqual(self.watch.server("boot-1", at(9)), [])
        self.assertEqual(self.watch.boot, "boot-2")
        self.assertEqual(len(self.watch.open), len(opened))

    def test_reboot_labels(self):
        self.assertEqual([self.watch.reboot_label(reason) for reason in (4, 5, 6, 7, 9, 14, 15, 1, 3)],
                         ["panic", "int_wdt", "task_wdt", "wdt", "brownout", "pwr_glitch", "cpu_lockup", None, None])
        self.watch.game(True, "game-1", at(0))
        self.assertEqual([self.watch.reboot_label(reason) for reason in (1, 2, 3)], ["poweron", "ext", None])

    def test_replay_reads_server_and_device_events(self):
        def server(kind, second, **fields):
            return {"source": "game_server", "event_type": kind, "occurred_utc": stamp(second),
                    "mac": fields.pop("mac", None), "fields": dict(fields, boot_id="boot-1")}
        events = [server("game_monitor_snapshot", 0, snapshot_complete=True, game_active=True, game_session_id="game-1")]
        for second in range(0, 25):
            events.append(server("request_ingress", second, mac=GLOVE))
            events += [server("request_ingress", second, mac=mac) for name, mac in GENERATORS.items() if name != "TG"]
        events.append({"source": "device", "event_type": "Boot", "mac": GENERATORS["SG"], "observed_utc": stamp(25),
                       "fields": {"text": "device.boot", "values": [4, 39, 1, 0]}})
        events.append({"source": "device", "event_type": "Boot", "mac": GENERATORS["SG"], "observed_utc": stamp(26),
                       "fields": {"text": "device.boot", "values": [3, 39, 1, 0]}})
        decisions = replay(events, GeneratorWatch(GENERATORS))
        self.assertEqual([(item["type"], item.get("names") or item.get("name")) for item in decisions],
                         [("open", ["TG"]), ("reboot", "SG")])

    def test_config(self):
        self.assertEqual(validate_generators({"enabled": False}),
                         {"enabled": False, "devices": {}, "silence_seconds": 20, "group_min": 3})
        self.assertEqual(validate_generators({"enabled": True, "devices": {"BG": "a4:f0:0f:d9:0b:cc"}})["devices"], {"BG": "a4:f0:0f:d9:0b:cc"})
        for bad in ({"enabled": True}, {"enabled": True, "devices": {"BG": "nope"}},
                    {"enabled": True, "devices": {"BG": GLOVE, "LG": GLOVE}}, {"devices": {}, "silence_seconds": 5},
                    {"devices": {}, "extra": 1}, {"enabled": "yes", "devices": {}}, {"devices": {"B G": GLOVE}}):
            with self.assertRaises(ValueError):
                validate_generators(bad)


def request(mac, second, seq, boot="server-1"):
    return Event(site_id="store-1", source="game_server", source_id="server-1", event_id=f"r{seq}",
                 observed_utc=stamp(second), occurred_utc=stamp(second), mac=mac, event_type="request_ingress",
                 fields={"boot_id": boot, "event_seq": seq, "request_type": "Loop"})


def snapshot(second, seq, active=True, session="game-1"):
    return Event(site_id="store-1", source="game_server", source_id="server-1", event_id=f"s{seq}",
                 observed_utc=stamp(second), occurred_utc=stamp(second), event_type="game_monitor_snapshot",
                 fields={"boot_id": "server-1", "event_seq": seq, "game_session_id": session if active else None,
                         "game_group": "g2" if active else None, "game_active": active, "snapshot_complete": True,
                         "participants": [{"device_id": "G2P4", "mac": GLOVE, "role": "player", "incident_id": None}] if active else [],
                         "active_incidents": []})


def boot(name, reason, second, *, device_boot=7, mac=None):
    return Event(site_id="store-1", source="device", source_id=mac or GENERATORS[name], event_id=f"ring:{device_boot}",
                 observed_utc=stamp(second), mac=mac or GENERATORS[name], device_id="HAS1_generator", event_type="Boot",
                 fields={"ring": "ring", "seq": device_boot, "boot": device_boot, "uptime_ms": 65,
                         "values": [reason, 39, 1, 0], "text": "device.boot"})


class IncidentFlowTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "incidents.db"
        self.store = self.open_store()
        self.seq = 0

    def tearDown(self):
        self.store.close()
        self.temp.cleanup()

    def open_store(self):
        store = IncidentStore(self.path)
        store.start_run("run-1")
        store.attach_generators(GeneratorWatch(GENERATORS), "server-1")
        return store

    def process(self, item, second):
        self.seq += 1
        self.store.process(self.seq, item, now=stamp(second), mono=100, run_id="run-1", channels=["slack", "notion"])
        self.store.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('delivery_run','run-1')")

    def traffic(self, start, end, *, silent=(), game=True):
        for second in range(start, end + 1):
            self.process(request(GLOVE, second, 1000 + self.seq), second)
            for name, mac in GENERATORS.items():
                if name not in silent:
                    self.process(request(mac, second, 1000 + self.seq), second)
            if second % 5 == 0:
                self.process(snapshot(second, 1000 + self.seq, active=game), second)

    def claim(self):
        return self.store.claim(run_id="run-1", mono=100, now_epoch=100)

    def deliver(self, row, remote_id=None):
        self.store.finish(row["id"], SimpleNamespace(state="delivered", retry_after=0, code="ok", remote_id=remote_id),
                          now_epoch=100, channels=["slack", "notion"])

    def incidents(self):
        return [tuple(row) for row in self.store.db.execute("SELECT device_id,state,reason FROM incident_state ORDER BY opened_utc")]

    def outbox(self):
        return [tuple(row) for row in self.store.db.execute("SELECT version,destination,state FROM incident_outbox ORDER BY id")]

    def test_silence_alerts_once_survives_snapshots_and_recovers(self):
        self.process(snapshot(0, 1), 0)
        self.traffic(0, 25, silent=("BG",))
        self.assertEqual(self.incidents(), [("BG", "open", "generator_silent")])
        row = self.claim()
        payload = row["payload"]
        self.assertEqual((payload["status"], payload["device_id"], payload["role"], payload["kind"], payload["gap_ms"]),
                         ("offline", "BG", "generator", "silence", 20000))
        self.deliver(row)
        self.deliver(self.claim())
        # Snapshots list no server incident for it, yet it stays open.
        self.traffic(26, 30, silent=("BG",))
        self.assertEqual(self.incidents(), [("BG", "open", "generator_silent")])
        self.process(request(GENERATORS["BG"], 31, 9999), 31)
        self.assertEqual(self.incidents(), [("BG", "recovered", "reconnected")])
        self.assertEqual([self.claim()["payload"]["status"] for _ in range(1)], ["recovered"])

    def test_generators_dropping_together_are_one_incident_and_end_with_the_game(self):
        self.process(snapshot(0, 1), 0)
        self.traffic(0, 3)
        self.traffic(4, 25, silent=tuple(GENERATORS))
        self.assertEqual(self.incidents(), [("발전기 BG·LG·SG·TG·UG", "open", "generator_silent")])
        self.assertEqual(self.claim()["payload"]["members"], ["BG", "LG", "SG", "TG", "UG"])
        self.process(snapshot(26, 9000, active=False), 26)
        self.assertEqual(self.incidents(), [("발전기 BG·LG·SG·TG·UG", "closed", "game_ended")])

    def test_server_pause_and_no_game_raise_nothing(self):
        self.traffic(0, 40, silent=("BG",), game=False)
        self.traffic(41, 45)
        self.traffic(80, 95)  # every device silent 46-79s: a server stall
        self.assertEqual(self.incidents(), [])

    def test_restarted_collector_resumes_recovery(self):
        self.process(snapshot(0, 1), 0)
        self.traffic(0, 25, silent=("LG",))
        self.store.close()
        self.store = self.open_store()
        self.process(request(GENERATORS["LG"], 26, 9999), 26)
        self.assertEqual(self.incidents(), [("LG", "recovered", "reconnected")])

    def test_crash_reboot_is_announced_once_with_blackbox_table(self):
        self.process(snapshot(0, 1, active=False), 0)
        self.process(boot("SG", 4, 10), 12)
        self.process(boot("SG", 4, 10), 13)  # importer reconciliation re-reads the same record
        self.assertEqual(self.incidents(), [("SG", "recovered", "generator_reboot")])
        claimed = {}
        for _ in range(2):
            row = self.claim()
            claimed[row["destination"]] = row["payload"]
            self.deliver(row, remote_id="abcdefab-1234-1234-1234-123456789abc" if row["destination"] == "notion" else None)
        self.assertEqual(set(claimed), {"slack", "notion"})
        payload = claimed["notion"]
        self.assertEqual((payload["status"], payload["reset_label"], payload["game_session_id"]),
                         ("rebooted", "panic", "게임외-20261025"))
        # No follow-up for a reboot; the Notion page gets the blackbox table.
        self.assertEqual(self.outbox(), [(1, "slack", "delivered"), (1, "notion", "delivered"), (3, "notion", "pending")])

    def test_reboots_that_are_not_announced(self):
        self.process(snapshot(0, 1, active=False), 0)
        self.process(boot("SG", 1, 10, device_boot=1), 10)   # power-on outside a game
        self.process(boot("SG", 3, 10, device_boot=2), 10)   # OTA / self-recovery restart
        self.process(boot("SG", 4, 10, device_boot=3), 400)  # old backlog
        self.process(boot("SG", 4, 10, device_boot=4, mac=GLOVE), 10)  # not a generator
        self.assertEqual(self.incidents(), [])
        self.process(snapshot(20, 2), 20)
        self.process(boot("SG", 1, 21, device_boot=5), 21)   # power-on during a game
        self.assertEqual([row[0] for row in self.incidents()], ["SG"])
        self.assertEqual(self.store.db.execute("SELECT session_id FROM incident_state").fetchone()[0], "game-1")


    def test_pending_silence_alert_is_canceled_when_it_recovers_first(self):
        self.process(snapshot(0, 1), 0)
        self.traffic(0, 25, silent=("BG",))
        self.process(request(GENERATORS["BG"], 26, 9999), 26)
        self.assertEqual(self.outbox(), [(1, "slack", "canceled"), (1, "notion", "canceled")])
        self.assertIsNone(self.claim())

    def test_untracked_generator_silence_ends_with_the_game(self):
        self.process(snapshot(0, 1), 0)
        self.store.db.execute("INSERT INTO incident_state VALUES ('lost','store-1','server-1','server-1','game-1','BG',NULL,'open','generator_silent',?,?,'{}')",
                              (stamp(0), stamp(0)))
        self.store.close()
        self.store = self.open_store()  # details unreadable: the watch cannot re-adopt it
        self.process(snapshot(5, 2), 5)
        self.assertEqual(self.incidents()[0][1], "open")
        self.process(snapshot(10, 3, active=False), 10)
        self.assertEqual(self.incidents(), [("BG", "closed", "game_ended")])

    def test_glove_incidents_are_unchanged_with_the_watch(self):
        glove = Event(site_id="store-1", source="game_server", source_id="server-1", event_id="off", observed_utc=stamp(1),
                      occurred_utc=stamp(1), mac=GLOVE, device_id="G2P4", event_type="device_offline",
                      fields={"boot_id": "server-1", "game_session_id": "game-1", "incident_id": "glove-1", "reason": "timeout", "event_seq": 2})
        self.process(glove, 1)
        listed = snapshot(2, 3)
        listed.fields["participants"][0]["incident_id"] = "glove-1"
        listed.fields["active_incidents"] = [{"device_id": "G2P4", "mac": GLOVE, "incident_id": "glove-1", "opened_utc": stamp(1)}]
        self.process(listed, 2)
        self.assertEqual(self.claim()["payload"]["device_id"], "G2P4")
        self.process(snapshot(3, 4), 3)  # no longer listed by the server
        self.assertEqual(self.incidents(), [("G2P4", "closed", "snapshot_no_longer_active")])

    def test_rebuilt_watch_redecides_a_rolled_back_event(self):
        self.process(snapshot(0, 1), 0)
        self.traffic(0, 19, silent=("BG",))
        with patch.object(IncidentStore, "_queue", side_effect=RuntimeError("injected")):
            with self.assertRaises(RuntimeError):
                self.process(request(GLOVE, 20, 8888), 20)
        self.assertEqual(self.incidents(), [])
        self.assertEqual(self.store.watch.open, {})  # rolled back with the transaction
        self.assertEqual(self.store.watch.session, "game-1")  # nothing else forgotten
        self.process(request(GLOVE, 20, 8888), 20)  # the runtime retries the same event
        self.assertEqual(self.incidents(), [("BG", "open", "generator_silent")])

    def test_routing_and_runtime_wiring(self):
        config = {"database": str(self.path), "notifications": {"enabled": False},
                  "generators": {"enabled": True, "devices": GENERATORS, "silence_seconds": 20, "group_min": 3}}
        runtime = IncidentRuntime(config, telemetry_database=str(self.path), site_id="store-1", source_id="server-1")
        state = runtime._new_store()
        try:
            self.assertIsNotNone(state.watch)
            self.assertTrue(runtime._routed(boot("SG", 4, 0), state))
            self.assertTrue(runtime._routed(request(GLOVE, 0, 1), state))
            other = Event(site_id="store-2", source="device", source_id=GENERATORS["SG"], event_id="x", observed_utc=stamp(0), mac=GENERATORS["SG"])
            self.assertFalse(runtime._routed(other, state))
            state.watch = None
            self.assertFalse(runtime._routed(boot("SG", 4, 0), state))
        finally:
            state.close()

    def test_reboot_alert_retries_and_ages_out(self):
        self.process(snapshot(0, 1, active=False), 0)
        self.process(boot("SG", 4, 10, device_boot=1), 10)
        self.process(boot("SG", 4, 20, device_boot=2), 20)  # crash loop: one announcement per 10 minutes
        self.assertEqual(len(self.incidents()), 1)
        row = self.claim()
        self.store.finish(row["id"], SimpleNamespace(state="retryable", retry_after=0, code="http_503", remote_id=None),
                          now_epoch=100, channels=["slack", "notion"])
        self.assertEqual(self.outbox()[0][2], "retryable")  # a reboot is never canceled for being "closed"
        self.store.db.execute("UPDATE incident_outbox SET next_attempt=0")
        late = at(10).timestamp() + 3601
        self.assertIsNone(self.store.claim(run_id="run-1", mono=100, now_epoch=late))
        self.assertEqual([state for _, _, state in self.outbox()], ["canceled", "canceled"])  # both at once

    def test_reboot_limit_does_not_cross_into_a_game(self):
        self.process(snapshot(0, 1, active=False), 0)
        self.process(boot("SG", 4, 10, device_boot=1), 10)
        self.process(snapshot(20, 2), 20)
        self.process(boot("SG", 1, 30, device_boot=2), 30)  # power-on during the game, 20s later
        self.assertEqual([row[0] for row in self.store.db.execute("SELECT session_id FROM incident_state ORDER BY opened_utc")],
                         ["게임외-20261025", "game-1"])

    def test_reboot_evidence_shows_the_boot_and_crash(self):
        telemetry = Path(self.temp.name) / "telemetry.db"
        store = TelemetryStore(telemetry)
        def record(seq, observed, kind, text, values, device_boot):
            return Event(site_id="store-1", source="device", source_id=GENERATORS["SG"], event_id=f"r1:{seq}",
                         observed_utc=observed, clock_quality="uptime_only", mac=GENERATORS["SG"], device_id="HAS1_generator",
                         event_type=kind, fields={"ring": "r1", "seq": seq, "boot": device_boot, "uptime_ms": 1000 * seq,
                                                  "values": list(values), "text": text})
        for seq in range(1, 6):
            store.save(record(seq, stamp(-60), "Health", "heap", (1, 2, 3, 4), 1))
        # One upload after the crash: Boot, the crash summary, then routine records, all received together.
        store.save(record(6, stamp(10), "Boot", "device.boot", (4, 39, 1, 0), 2))
        store.save(record(7, stamp(10), "Crash", "prior.core:mqtt_task", (0x4013017c, 28, 0, 2), 2))
        for seq in range(8, 16):
            store.save(record(seq, stamp(10), "Health", "heap", (1, 2, 3, 4), 2))
        store.close()
        self.process(snapshot(0, 1, active=False), 0)
        self.process(record(6, stamp(10), "Boot", "device.boot", (4, 39, 1, 0), 2), 10)
        incident = self.store.db.execute("SELECT * FROM incident_state").fetchone()
        with closing(_open_telemetry(telemetry)) as raw:
            evidence = summarize(raw, incident, stamp(400))
        text = str(evidence["rows"])
        self.assertIn("재부팅", text)
        self.assertIn("크래시", text)
        self.assertNotIn("기록 없음", " ".join(evidence["highlights"]))


class CardTests(unittest.TestCase):
    REBOOT = {"incident_id": "incident-r", "site_id": "store-1", "device_id": "SG", "device_mac": "A4F00FD90A74",
              "game_session_id": "게임외-20261025", "status": "rebooted", "role": "generator", "kind": "reboot",
              "reset_label": "panic", "firmware": 39, "occurred_at": stamp(0), "opened_at": stamp(0),
              "close_reason": "generator_reboot"}

    def test_reboot_cards(self):
        text = str(slack_message(self.REBOOT))
        self.assertIn("🔁 장치 재부팅", text)
        self.assertIn("패닉(크래시)", text)
        self.assertIn("재부팅", slack_event_reply(self.REBOOT, {})["text"])
        card = slack_session_card(self.REBOOT, {"incidents": [{"device_id": "SG", "state": "recovered", "reason": "generator_reboot",
                                                              "opened_utc": stamp(0), "updated_utc": stamp(0)}]})
        self.assertIn("🔁 SG", str(card))
        properties = _event_properties(self.REBOOT, "rebooted", "incident-r")
        self.assertEqual(properties["이벤트"]["title"][0]["text"]["content"], "SG 재부팅")
        self.assertEqual(properties["상태"]["status"]["name"], "복구됨")
        self.assertEqual(properties["종료 사유"]["select"]["name"], "재부팅 패닉(크래시)")
        self.assertNotIn("지속(초)", properties)

    def test_reboot_is_threaded_so_someone_is_notified(self):
        calls = []
        def call(url, method, body, token):
            calls.append((url.rsplit("/", 1)[-1], body.get("thread_ts")))
            return {"status": 200, "body": '{"ok": true, "ts": "1700000000.000100"}'}
        result = _send_slack_session(call, "xoxb-synthetic-token", "C0123456789", self.REBOOT)
        self.assertEqual(result.state, "delivered")
        self.assertEqual(calls, [("chat.postMessage", None), ("chat.postMessage", "1700000000.000100")])

    def test_game_end_close_reason_is_readable(self):
        payload = dict(self.REBOOT, status="monitoring_ended", close_reason="game_ended", kind="silence")
        self.assertIn("게임 종료", slack_event_reply(payload, {})["text"])


if __name__ == "__main__":
    unittest.main()
