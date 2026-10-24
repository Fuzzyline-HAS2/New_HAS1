"""Incident policy tests: no alarms from MQTT silence, battery swaps or replay."""
from datetime import timedelta
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import threading
import time
import unittest
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from incidents import IncidentStore, IncidentRuntime, validate_config
from telemetry import Event, TelemetryStore, parse_utc, utc_now

NOW = "2026-10-24T03:00:00.000000+00:00"
MAC = "AABBCCDDEEFF"


def event(kind="game_monitor_snapshot", incident="incident-1", active=True, session="game-1", boot="server-1", seq=1, when=NOW, observed=None, complete=True, mac=MAC, **changes):
    fields = {"boot_id":boot,"game_session_id":session,"game_group":"g1","event_seq":seq}
    if kind == "game_monitor_snapshot":
        fields.update(game_active=active,snapshot_complete=complete,
          participants=[{"device_id":"glove-1","mac":mac,"role":"ghost","incident_id":incident}] if active else [],
          active_incidents=[{"device_id":"glove-1","mac":mac,"incident_id":incident,"opened_utc":when}] if active and incident else [])
    else:
        fields.update(incident_id=incident,reason="reconnected" if kind=="device_recovered" else "game_ended" if kind=="alarm_cleared" else "timeout")
    fields.update(changes)
    return Event(site_id="store-1",source="game_server",source_id="server-1",event_id=str(seq)+kind,observed_utc=observed or when,occurred_utc=when,mac=mac,device_id="glove-1",event_type=kind,fields=fields)


class PolicyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name)/"incidents.db"
        self.store = IncidentStore(self.path)
        self.store.start_run("run-1")
        self.seq = 0

    def tearDown(self):
        self.store.close()
        self.temp.cleanup()

    def process(self, item, now=NOW, mono=100):
        self.seq += 1
        self.store.process(self.seq,item,now=now,mono=mono,run_id="run-1",channels=["slack","notion"])
        self.store.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('delivery_run','run-1')")

    def claim(self, mono=100):
        return self.store.claim(run_id="run-1",mono=mono,now_epoch=100)

    def delivered(self, row):
        self.store.finish(row["id"],SimpleNamespace(state="delivered",retry_after=0,code="ok",remote_id=None),now_epoch=100,channels=["slack","notion"])

    def states(self):
        return [tuple(row) for row in self.store.db.execute("SELECT version,destination,state FROM incident_outbox ORDER BY id")]

    def test_battery_swap_idle_reboot_and_mqtt_silence_do_not_alarm(self):
        self.process(event(active=False,session=None))
        self.process(Event(site_id="store-1",source="device",source_id="glove",event_id="boot",observed_utc=NOW,mac=MAC,event_type="boot",fields={"boot":2}))
        self.assertIsNone(self.claim())
        self.assertEqual(self.states(),[])

    def test_offline_needs_snapshot_and_is_deduplicated(self):
        self.process(event("device_offline"))
        self.assertIsNone(self.claim())
        self.process(event(seq=2))
        self.process(event(seq=3))
        self.assertEqual(len(self.states()),2)
        self.assertEqual(self.claim()["payload"]["status"],"offline")

    def test_old_snapshot_reobserved_now_and_partial_are_not_fresh(self):
        self.process(event(when="2026-10-24T02:00:00Z",observed=NOW))
        self.process(event(seq=2,complete=False))
        self.assertEqual(self.states(),[])

    def test_backlog_caughtup_gate(self):
        self.process(event())
        self.store.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('delivery_run','')")
        self.assertIsNone(self.claim())
        self.process(event(active=False,session=None,seq=2))
        self.assertIsNone(self.claim())
        self.assertTrue(all(row[2]=="canceled" for row in self.states()))

    def test_restart_needs_new_snapshot_and_ambiguous_send_manual_review(self):
        self.process(event())
        self.claim()
        self.store.close()
        self.store = IncidentStore(self.path)
        self.store.start_run("run-2")
        self.assertIsNone(self.claim())
        self.assertEqual(self.states()[0][2],"manual_review")
        self.store.process(2,event(seq=2),now=NOW,mono=200,run_id="run-2",channels=["slack","notion"])
        self.store.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('delivery_run','run-2')")
        self.assertIsNotNone(self.store.claim(run_id="run-2",mono=200,now_epoch=100))

    def test_stale_source_and_clock_step_fail_closed(self):
        self.process(event())
        self.assertIsNone(self.claim(mono=131))
        self.store.invalidate("clock_step")
        self.assertIsNone(self.claim())
        self.process(event(seq=2))
        self.assertIsNotNone(self.claim())

    def test_only_sent_offline_receives_actual_recovery(self):
        self.process(event())
        row = self.claim()
        self.delivered(row)
        self.process(event("device_recovered",seq=2))
        self.assertIn((2,"slack","pending"),self.states())
        self.assertNotIn((2,"notion","pending"),self.states())
        self.assertEqual(self.claim()["payload"]["status"],"recovered")

    def test_game_end_does_not_pretend_recovery(self):
        self.process(event())
        self.delivered(self.claim())
        self.delivered(self.claim())
        self.process(event("alarm_cleared",seq=2))
        row = self.claim()
        self.assertEqual(row["destination"],"notion")
        self.assertEqual(row["payload"]["status"],"monitoring_ended")

    def test_missed_end_snapshot_and_next_samegroup_newsession(self):
        self.process(event())
        self.process(event(seq=2,incident="incident-2",session="game-2"))
        old = self.store.db.execute("SELECT state FROM incident_state WHERE incident_id='incident-1'").fetchone()[0]
        self.assertEqual(old,"closed")
        self.assertEqual(self.claim()["incident_id"],"incident-2")

    def test_incomplete_inventory_and_outoforder_snapshot_do_not_override(self):
        self.process(event(seq=10,active=False,session=None))
        self.process(event(seq=9))
        self.process(event(seq=11,participants=[]))
        self.assertEqual(self.states(),[])

    def test_new_incomplete_snapshot_revokes_previous_lease(self):
        self.process(event(seq=10))
        self.process(event(seq=12,complete=False))
        self.assertIsNone(self.claim())
        self.process(event(seq=11))
        self.assertIsNone(self.claim())
        self.process(event(seq=13))
        self.assertIsNotNone(self.claim())

    def test_new_ambiguous_mac_binding_downgrades_correlation(self):
        self.process(event(seq=1))
        self.process(event(seq=2,mac=None))
        self.assertIsNone(self.store.export("incident-1")["incident"]["mac"])
        self.assertIsNone(self.claim()["payload"]["device_mac"])

    def test_contradictory_mac_and_dangling_participant_fail_closed(self):
        self.process(event(seq=1))
        self.process(event(seq=2,active_incidents=[{"device_id":"glove-1","incident_id":"incident-1","mac":"112233445566","opened_utc":NOW}]))
        self.assertIsNone(self.claim())
        self.process(event(seq=3))
        self.process(event(seq=4,active_incidents=[]))
        self.assertIsNone(self.claim())

    def test_stale_source_queue_does_not_starve_other_source(self):
        self.process(event())
        # Simulate retained incidents from another source/site configuration.
        for index in range(130):
            name="stale-"+str(index)
            self.store.db.execute("INSERT INTO incident_state SELECT ?,site_id,'stale-source',boot_id,session_id,device_id,mac,state,reason,opened_utc,updated_utc,details FROM incident_state WHERE incident_id='incident-1'",(name,))
            self.store.db.execute("INSERT INTO incident_outbox(incident_id,destination,version,payload) VALUES (?,'slack',1,'{}')",(name,))
        self.store.db.execute("UPDATE incident_outbox SET state='canceled' WHERE incident_id='incident-1'")
        self.process(event(seq=2,incident="latest-incident"))
        self.assertEqual(self.claim()["incident_id"],"latest-incident")

    def test_export_identity_filter_precedes_limit(self):
        self.process(event())
        path=Path(self.temp.name)/"telemetry.db"
        raw=TelemetryStore(path)
        for seq in range(10):
            raw.save(event("http_request",seq=seq,incident="other-incident",mac="112233445566"))
        raw.save(event("device_offline",seq=20))
        raw.close()
        report=self.store.export("incident-1",path,limit=1)
        self.assertEqual(len(report["related_events"]),1)
        self.assertEqual(report["related_events"][0]["basis"],"explicit_incident_id")
        self.assertFalse(report["truncated"])

    def test_readonly_list_stats_and_export_do_not_mutate(self):
        self.process(event())
        reader=IncidentStore(self.path,readonly=True)
        self.assertEqual(reader.list_incidents(1)[0]["incident_id"],"incident-1")
        self.assertEqual(reader.statistics()["incidents_by_state"],{"open":1})
        with self.assertRaises(sqlite3.OperationalError):
            reader.db.execute("DELETE FROM incident_state")
        reader.close()

    def test_late_recovery_after_monitoring_end_does_not_claim_recovery(self):
        self.process(event())
        self.delivered(self.claim())
        self.process(event("alarm_cleared",seq=2))
        self.process(event("device_recovered",seq=3))
        self.assertFalse(any(row[0]==2 for row in self.states()))

    def test_retry_ceiling_and_ambiguous_send_not_automatically_retried(self):
        self.process(event())
        row=self.claim()
        self.store.finish(row["id"],SimpleNamespace(state="unknown",retry_after=0,code="timeout",remote_id=None),now_epoch=100,channels=["slack"])
        self.assertEqual(self.states()[0][2],"manual_review")
        row=self.claim()
        self.store.db.execute("UPDATE incident_outbox SET attempts=12 WHERE id=?",(row["id"],))
        self.store.finish(row["id"],SimpleNamespace(state="retryable",retry_after=30,code="http_429",remote_id=None),now_epoch=100,channels=["notion"])
        self.assertEqual(self.states()[1][2],"manual_review")

    def test_unknown_mac_and_delayed_flash_are_not_certain_correlations(self):
        self.process(event())
        path=Path(self.temp.name)/"telemetry.db"
        raw=TelemetryStore(path)
        raw.save(event("device_offline"))
        raw.save(Event(site_id="store-1",source="device",source_id="glove",event_id="flash",observed_utc=NOW,mac=MAC,event_type="wifi_disconnected",fields={"boot":123,"uptime_ms":12}))
        raw.save(Event(site_id="store-1",source="omada",source_id="ap",event_id="ap",observed_utc=NOW,occurred_utc=NOW,mac=MAC,event_type="disconnect"))
        raw.save(Event(site_id="store-1",source="omada",source_id="ap",event_id="name_only",observed_utc=NOW,occurred_utc=NOW,device_id="glove-1",event_type="disconnect"))
        raw.close()
        report=self.store.export("incident-1",path)
        self.assertEqual(report["root_cause"],"not_determined")
        self.assertEqual({r["basis"] for r in report["related_events"]},{"explicit_incident_id","same_mac_time_unknown","same_mac_time_window"})
        self.assertFalse(any(r["event"]["event_id"]=="name_only" for r in report["related_events"]))


class MigrationTests(unittest.TestCase):
    def test_ingest_cursor_migrates_once_and_late_arrival_gets_new_seq(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/"telemetry.db"
            raw=TelemetryStore(path)
            raw.save(event(seq=1))
            raw.db.execute("DROP TABLE telemetry_ingest")
            raw.db.execute("PRAGMA user_version=2")
            raw.close()
            raw=TelemetryStore(path)
            self.assertEqual(raw.db.execute("SELECT count(*) FROM telemetry_ingest").fetchone()[0],1)
            raw.save(event(seq=2,when="2026-10-23T00:00:00Z"))
            self.assertEqual(raw.db.execute("SELECT event_id FROM telemetry_ingest ORDER BY seq DESC LIMIT 1").fetchone()[0],"2game_monitor_snapshot")
            self.assertFalse(raw.save(event(seq=1)))
            self.assertEqual(raw.db.execute("SELECT count(*) FROM telemetry_ingest").fetchone()[0],2)
            raw.close()
            raw=TelemetryStore(path)
            self.assertEqual(raw.db.execute("SELECT max(seq) FROM telemetry_ingest").fetchone()[0],2)
            raw.close()

    def test_snapshot_extra_budget_not_available_to_other_events(self):
        snapshot=event(padding="x"*20000)
        self.assertEqual(snapshot.event_type,"game_monitor_snapshot")
        with self.assertRaises(ValueError):
            event("device_offline",padding="x"*20000)
        with self.assertRaises(ValueError):
            event(padding="x"*33000)

    def test_incident_database_cannot_open_raw_database(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/"telemetry.db"
            TelemetryStore(path).close()
            with self.assertRaises(sqlite3.DatabaseError):
                IncidentStore(path)

    def test_raw_event_and_ingest_cursor_commit_atomically(self):
        with tempfile.TemporaryDirectory() as folder:
            raw=TelemetryStore(Path(folder)/"telemetry.db")
            raw.db.execute("CREATE TRIGGER reject_ingest BEFORE INSERT ON telemetry_ingest BEGIN SELECT RAISE(FAIL,'injected'); END")
            with self.assertRaises(sqlite3.DatabaseError):
                raw.save(event())
            self.assertEqual(raw.event_count("store-1"),0)
            self.assertEqual(raw.db.execute("SELECT count(*) FROM event_times").fetchone()[0],0)
            raw.close()


class RuntimeTests(unittest.TestCase):
    def test_processing_fault_revokes_in_memory_and_persisted_authorization(self):
        with tempfile.TemporaryDirectory() as folder:
            folder=Path(folder)
            telemetry_path=folder/"telemetry.db"
            raw=TelemetryStore(telemetry_path)
            raw.save(event(when=utc_now()))
            fail=threading.Event()
            def disk_check():
                if fail.is_set():
                    raise OSError("injected disk failure")
            runtime=IncidentRuntime({"database":str(folder/"incidents.db"),"notifications":{"enabled":False}},telemetry_database=telemetry_path,site_id="store-1",source_id="server-1",check_disk=disk_check)
            runtime.start()
            try:
                end=time.monotonic()+4
                while time.monotonic()<end and runtime.health!="healthy":
                    time.sleep(.01)
                self.assertEqual(runtime.health,"healthy")
                fail.set()
                end=time.monotonic()+4
                while time.monotonic()<end and runtime.health!="processing_error":
                    time.sleep(.01)
                self.assertEqual(runtime.health,"processing_error")
                self.assertFalse(runtime.authorized.is_set())
                reader=IncidentStore(folder/"incidents.db",readonly=True)
                self.assertEqual(reader.db.execute("SELECT value FROM incident_meta WHERE key='delivery_run'").fetchone()[0],"")
                self.assertIsNone(reader.db.execute("SELECT validated_run FROM incident_sources").fetchone()[0])
                reader.close()
            finally:
                self.assertTrue(runtime.stop())
                raw.close()

    def test_caughtup_replay_and_workers_keep_notification_outside_ingest(self):
        with tempfile.TemporaryDirectory() as folder:
            folder=Path(folder)
            telemetry_path=folder/"telemetry.db"
            raw=TelemetryStore(telemetry_path)
            now=utc_now()
            for seq in range(300):
                raw.save(Event(site_id="store-1",source="collector",source_id="pc",event_id=str(seq),observed_utc=now,event_type="health"))
            raw.save(event(seq=301,when=now))
            raw.save(event(seq=302,when=now,active=False,session=None))
            settings={"database":str(folder/"incidents.db"),"notifications":{"enabled":True,"slack":{"enabled":True}}}
            runtime=IncidentRuntime(settings,telemetry_database=telemetry_path,site_id="store-1",source_id="server-1")
            calls=[]
            with patch("notifications.NotificationSender") as sender:
                sender.return_value.send.side_effect=lambda *args: calls.append(args) or SimpleNamespace(state="delivered",retry_after=0,code="ok",remote_id=None)
                runtime.start()
                end=time.monotonic()+4
                while time.monotonic()<end:
                    if runtime.health=="healthy":
                        break
                    time.sleep(.01)
                self.assertEqual(runtime.health,"healthy")
                self.assertEqual(calls,[])
                raw.save(event(seq=303,when=utc_now(),incident="next-incident",session="game-2"))
                end=time.monotonic()+4
                while time.monotonic()<end and not calls:
                    time.sleep(.01)
                self.assertEqual(len(calls),1)
                self.assertEqual(calls[0][1]["incident_id"],"next-incident")
                self.assertTrue(runtime.stop())
            raw.close()

    def test_config_requires_server_and_distinct_databases(self):
        with tempfile.TemporaryDirectory() as folder:
            base=Path(folder)
            server={"enabled":False}
            with self.assertRaises(ValueError):
                validate_config({"enabled":True},base,base/"telemetry.db",base/"device.db",server)
            server["enabled"]=True
            with self.assertRaises(ValueError):
                validate_config({"enabled":True,"database":"telemetry.db"},base,base/"telemetry.db",base/"device.db",server)
            result=validate_config({"enabled":True},base,base/"telemetry.db",base/"device.db",server)
            self.assertFalse(result["notifications"]["enabled"])


if __name__=="__main__":
    unittest.main()
