import io
from contextlib import closing
import json
from pathlib import Path
import sqlite3
import socket
import sys
import tempfile
import time
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from helpers import MAC,RING,TOPIC,payload,record
from protocol import parse_batch,decode_record,named_values
from storage import Store
from collector import Collector
from clock_probe import ClockProbe
from evidence_runtime import DeviceImporter
from export import export
from import_omada_csv import import_csv
from telemetry import TelemetryStore
from telemetry_runtime import TelemetryRuntime,validate_config
from timeline import align_event,event_interval,device_event,Anchor


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.folder=Path(self.temp.name)
    def tearDown(self): self.temp.cleanup()

    def test_old_database_migration_and_duplicate_preserves_unknown_arrival(self):
        path=self.folder/'raw.db';db=sqlite3.connect(path)
        db.execute('CREATE TABLE records(mac TEXT,ring TEXT,seq INTEGER,device TEXT,raw BLOB,received_utc TEXT,PRIMARY KEY(mac,ring,seq)) WITHOUT ROWID')
        db.execute('INSERT INTO records VALUES(?,?,?,?,?,?)',(MAC,RING,1,'iotglove',record(1),'2026-10-24T00:00:00+00:00'))
        db.execute('PRAGMA user_version=1');db.commit();db.close()
        store=Store(path)
        store.save(parse_batch(TOPIC,payload((1,2))),arrival_utc='2026-10-24T00:01:00+00:00',queue_latency_ms=7)
        old=store.db.execute('SELECT arrival_utc,persisted_utc,queue_latency_ms FROM records WHERE seq=1').fetchone()
        new=store.db.execute('SELECT arrival_utc,queue_latency_ms FROM records WHERE seq=2').fetchone()
        self.assertEqual(old,(None,None,None));self.assertEqual(new,('2026-10-24T00:01:00+00:00',7))
        store.close()
        reopened=Store(path);self.assertEqual(reopened.db.execute('PRAGMA user_version').fetchone()[0],2);reopened.close()

    def test_arrival_queue_timing_is_persisted_before_ack(self):
        path=self.folder/'raw.db';seen=[]
        def publish(*_):
            with closing(sqlite3.connect(path)) as db: seen.append(db.execute('SELECT arrival_utc,persisted_utc,queue_latency_ms FROM records').fetchone())
            return True
        collector=Collector(path,publish);collector.start();collector.submit(TOPIC,payload());collector.close()
        self.assertTrue(seen[0][0]);self.assertTrue(seen[0][1]);self.assertGreaterEqual(seen[0][2],0)

    def test_import_restarts_catches_late_lower_rows_and_other_identity(self):
        raw=Store(self.folder/'raw.db');journal=TelemetryStore(self.folder/'timeline.db')
        try:
            raw.save(parse_batch(TOPIC,payload((2,4))))
            importer=DeviceImporter(raw.path,journal,'store-1',page_size=1)
            for _ in range(6): importer.poll()
            raw.save(parse_batch(TOPIC,payload((1,3))))
            importer=DeviceImporter(raw.path,journal,'store-1',page_size=1)
            for _ in range(14): importer.poll()
            self.assertEqual(journal.event_count('store-1'),4)
            self.assertEqual(raw.db.execute('SELECT count(*) FROM records').fetchone()[0],4)
            self.assertEqual(importer.coverage()['cursors'][0]['highest_imported_seq'],4)
        finally: raw.close();journal.close()

    def test_new_tail_priority_while_large_history_reconciles(self):
        raw=Store(self.folder/'raw.db');journal=TelemetryStore(self.folder/'timeline.db')
        try:
            # Simulate a durable prior import high-water mark without spending
            # this regression test fsync'ing ten thousand already known rows.
            raw.db.execute("BEGIN")
            raw.db.executemany("INSERT INTO records(mac,ring,seq,device,raw,received_utc) VALUES(?,?,?,?,?,?)",
                ((MAC,RING,n,'iotglove',record(n),'2026-10-24T00:00:00Z') for n in range(2,10001)))
            raw.db.execute("COMMIT")
            importer=DeviceImporter(raw.path,journal,'store-1',page_size=16)
            journal.db.execute("INSERT INTO device_import_cursor VALUES(?,?,?,?,?,?)",('store-1',str(raw.path),MAC,RING,0,10000))
            raw.save(parse_batch(TOPIC,payload((1,10001))))
            importer.poll()
            ids={e.event_id for e in journal.events('store-1')}
            self.assertIn(RING+':10001',ids)
            self.assertIn(RING+':1',ids)
            self.assertLessEqual(importer.counts['examined'],32)
        finally:raw.close();journal.close()

    def test_import_cursor_not_advanced_after_destination_failure(self):
        raw=Store(self.folder/'raw.db');journal=TelemetryStore(self.folder/'timeline.db')
        try:
            raw.save(parse_batch(TOPIC,payload((1,2))))
            importer=DeviceImporter(raw.path,journal,'store-1');save=journal.save
            journal.save=lambda _: (_ for _ in ()).throw(OSError('full'))
            with self.assertRaises(OSError): importer.poll()
            self.assertEqual(importer.coverage()['cursors'],[])
            journal.save=save
            importer=DeviceImporter(raw.path,journal,'store-1');importer.poll()
            self.assertEqual(journal.event_count('store-1'),2)
        finally: raw.close();journal.close()

    def test_export_latest_cursor_window_and_named_unsigned_values(self):
        raw=Store(self.folder/'raw.db')
        try:
            raw.save(parse_batch(TOPIC,payload((1,2,3))))
            out=io.StringIO();meta=export(raw.path,out,limit=2,latest=True)
            self.assertEqual([r['seq'] for r in map(json.loads,out.getvalue().splitlines())],[3,2]);self.assertTrue(meta['truncated'])
            out=io.StringIO();meta=export(raw.path,out,limit=2,latest=True,after=meta['next_cursor'])
            self.assertEqual(json.loads(out.getvalue())['seq'],1);self.assertFalse(meta['truncated'])
            with self.assertRaises(ValueError): export(raw.path,io.StringIO(),after='bad')
            self.assertEqual(export(raw.path,io.StringIO(),window_start='2000-01-01T00:00:00Z',window_end='2000-01-02T00:00:00Z')['count'],0)
            decoded=named_values(decode_record(record(kind=3,text=b'wifi.ip.gw.mask',values=(-1407319765,0,-256,1))))
            self.assertEqual(decoded['fields']['netmask']['display'],'255.255.255.0')
            self.assertEqual(named_values(decode_record(record(text=b'new-tag')))['schema'],'unknown')
        finally: raw.close()

    def probe(self):
        self.clock=[10.0,'2026-10-24T00:00:00+00:00'];sent=[];saved=[]
        probe=ClockProbe('store-1',lambda t,p: sent.append(p) or True,lambda a:saved.append(a) or True,lambda:self.clock[0],lambda:self.clock[1])
        probe.challenge(parse_batch(TOPIC,payload()))
        token=sent[0].decode().split()[1]
        reply={'v':1,'kind':'time_probe','mac':MAC,'ring':RING,'boot':3,'token':token,'uptime_ms':1000,'seq_min':1,'seq_max':10,'record_uptime_wraps':0}
        return probe,reply,sent,saved

    def test_probe_measured_bracket_unverified_pc_not_certified_utc(self):
        probe,reply,sent,saved=self.probe();self.clock[:]=[10.02,'2026-10-24T00:00:00.020+00:00']
        probe.receive(TOPIC,json.dumps(reply).encode());self.assertEqual(len(saved),1)
        self.assertEqual(saved[0].clock_quality,'pc_unverified')
        event=device_event('store-1',MAC,RING,1,'iotglove',record(1),'2026-10-25T00:00:00Z')
        # Fixture boot differs: never align an old boot to current probe.
        self.assertIsNone(align_event(event,saved)['occurred_utc'])
        from dataclasses import replace
        current=replace(event,fields=dict(event.fields,boot=3,uptime_ms=1000))
        aligned=align_event(current,saved)
        self.assertEqual(aligned['clock_quality'],'pc_unverified')
        self.assertEqual(aligned['alignment']['interval_scope'],'collector_pc_clock')
        self.assertIsNone(aligned['alignment']['reference_clock_offset_ms'])
        self.assertIsNone(event_interval(aligned))
        probe.receive(TOPIC,json.dumps(reply).encode());self.assertEqual(len(saved),1)
        probe.challenge(parse_batch(TOPIC,payload()));self.assertEqual(len(sent),1)

    def test_probe_rejects_wrong_identity_expiry_step_wrap_and_bool(self):
        for change,clock in [({'mac':'000000000000'},None),({},[13,'2026-10-24T00:00:03Z']),({},[10.1,'2026-10-24T01:00:00Z']),({'record_uptime_wraps':1},None),({'boot':True},None)]:
            probe,reply,_,saved=self.probe();reply.update(change)
            if clock:self.clock[:]=clock
            probe.receive(TOPIC,json.dumps(reply).encode());self.assertEqual(saved,[])

    def test_probe_never_replaces_or_delays_durable_ack_on_failure(self):
        store=Store(self.folder/'raw.db');sent=[]
        try:
            collector=Collector(store.path,lambda _,p:sent.append(p) or True)
            collector.clock_probe=type('Broken',(),{'challenge':lambda *_:(_ for _ in ()).throw(OSError('failed'))})()
            self.assertTrue(collector.process(store,TOPIC,payload()))
            self.assertTrue(sent[0].startswith(b'ACK1 '));self.assertEqual(collector.counts['clock_probe_errors'],1)
        finally:store.close()

    def test_csv_explicit_mapping_dedup_and_naive_time_rejected(self):
        source=self.folder/'omada.csv';source.write_text('When,Description\n2026-10-24T09:00:00+09:00,Client aa-bb-cc-dd-ee-ff disconnected\n')
        store=TelemetryStore(self.folder/'timeline.db')
        try:
            mapping={'message':'Description','timestamp':'When'}
            self.assertEqual(import_csv(store,source,'store-1','172.30.1.1',mapping)['inserted'],1)
            # Import capture-time-dependent parser details must not change stable facts.
            time.sleep(.01)
            self.assertEqual(import_csv(store,source,'store-1','172.30.1.1',mapping)['duplicates'],1)
            event=next(store.events('store-1'));self.assertEqual(event.occurred_utc,'2026-10-24T00:00:00.000000+00:00')
            source.write_text('When,Description\n2026-10-24T09:00:00,x\n')
            with self.assertRaises(ValueError):import_csv(store,source,'store-1','172.30.1.1',mapping)
        finally:store.close()

    def test_gateway_config_rejects_wrong_site_before_runtime(self):
        config={'database':str(self.folder/'raw.db'),'telemetry':{'site_id':'store-2','database':'timeline.db',
            'gateway_udp':{'enabled':True,'host':'127.0.0.1','port':5515,'source_id':'gateway'}}}
        with self.assertRaisesRegex(ValueError,'store-1'):
            validate_config(config,self.folder)

    def test_gateway_bind_failure_recovers_after_port_released(self):
        blocker=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        blocker.bind(('127.0.0.1',0));port=blocker.getsockname()[1]
        settings={'site_id':'store-1','database':str(self.folder/'timeline.db'),'min_free_bytes':16*1024**2,
                  'omada':{'enabled':False},'server_jsonl':{'enabled':False},
                  'gateway_udp':{'enabled':True,'host':'127.0.0.1','port':port,'source_id':'store-1-php-gateway'}}
        runtime=TelemetryRuntime(settings)
        try:
            runtime.start();self.assertFalse(runtime.gateway.is_receiving())
            blocker.close();runtime._gateway_checked=-100
            deadline=time.monotonic()+3
            while time.monotonic()<deadline and not runtime.gateway.is_receiving():time.sleep(.01)
            self.assertTrue(runtime.gateway.is_receiving())
            self.assertEqual(runtime.counts['gateway_restarts'],1)
        finally:
            blocker.close();self.assertTrue(runtime.stop())
        self.assertFalse(runtime.gateway.is_receiving())

    def test_health_persisted_and_missing_device_db_is_reported(self):
        settings={'site_id':'store-1','database':str(self.folder/'timeline.db'),'min_free_bytes':16*1024**2,'omada':{'enabled':False},'server_jsonl':{'enabled':False},'device_import':{'enabled':True}}
        runtime=TelemetryRuntime(settings,device_database=self.folder/'missing.db',collector_stats=lambda:{'worker_alive':True,'queue_size':3})
        runtime.start();time.sleep(.15);self.assertTrue(runtime.stop())
        store=TelemetryStore(settings['database'])
        try:
            events=list(store.events('store-1'));self.assertEqual(len(events),2)
            start=next(e for e in events if e.event_type=='collector_started')
            self.assertEqual(start.fields['runtime']['device_import_errors'],1)
            self.assertEqual(start.fields['device_collector']['queue_size'],3)
        finally:store.close()


if __name__=='__main__':unittest.main()
