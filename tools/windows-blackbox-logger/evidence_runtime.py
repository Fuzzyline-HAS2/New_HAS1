"""Bounded, replayable evidence maintenance, never in the device ACK worker."""
import sqlite3
from contextlib import closing
from pathlib import Path
from protocol import decode_record
from retention import retention_floors, skip_pruned_device
from timeline import device_event


class DeviceImporter:
    """One identity/page per poll with durable scan cursors and repeated reconciliation.

    Re-scanning completed ranges catches late lower sequence arrivals. Checking the
    destination identity before decoding avoids rewriting known events. Cursor
    updates follow destination commits; interruption can only cause safe replay.
    """
    def __init__(self, filename, store, site_id, page_size=128):
        self.filename, self.store, self.site_id = str(Path(filename).resolve()), store, site_id
        self.page_size = page_size
        self.next_identity = ("", "")
        self.counts = {"inserted": 0, "examined": 0, "cycles": 0, "pruned_skipped": 0}
        store.db.execute("CREATE TABLE IF NOT EXISTS device_import_cursor (site TEXT,filename TEXT,mac TEXT,ring TEXT,scan_seq INTEGER,high_seq INTEGER,PRIMARY KEY(site,filename,mac,ring)) WITHOUT ROWID")

    def poll(self):
        with closing(sqlite3.connect(Path(self.filename).as_uri() + "?mode=ro", uri=True, timeout=0.1)) as db:
            identity = db.execute("SELECT mac,ring FROM records WHERE (mac,ring)>(?,?) ORDER BY mac,ring LIMIT 1", self.next_identity).fetchone()
            if identity is None:
                self.next_identity = ("", "")
                self.counts["cycles"] += 1
                return False
            self.next_identity = identity
            key = (self.site_id, self.filename, *identity)
            cursor = self.store.db.execute("SELECT scan_seq,high_seq FROM device_import_cursor WHERE site=? AND filename=? AND mac=? AND ring=?", key).fetchone() or (0, 0)
            # Prioritize new tail rows even during reconciliation of large old
            # histories. A second independently bounded page catches late rows.
            tail = db.execute("SELECT mac,ring,seq,device,raw,received_utc FROM records WHERE mac=? AND ring=? AND seq>? ORDER BY seq LIMIT ?", (*identity, cursor[1], self.page_size)).fetchall()
            reconcile = db.execute("SELECT mac,ring,seq,device,raw,received_utc FROM records WHERE mac=? AND ring=? AND seq>? AND seq<=? ORDER BY seq LIMIT ?", (*identity, cursor[0],cursor[1],self.page_size)).fetchall()
            last, high = cursor
            # Reconciliation rescans old ranges; records retention removed stay removed.
            floors = retention_floors(self.store.db, self.site_id)
            for row in tail + reconcile:
                address, ring, seq, device, raw, received = row
                previous = self.store.db.execute("SELECT 1 FROM telemetry_events WHERE site_id=? AND source='device' AND source_id=? AND event_id=?", (self.site_id,address,ring+":"+str(seq))).fetchone()
                if not previous and skip_pruned_device(seq, cursor[1], raw, received, floors):
                    self.counts["pruned_skipped"] += 1
                elif not previous:
                    self.store.save(device_event(self.site_id, address, ring, seq, device, raw, received))
                    self.counts["inserted"] += 1
                high = max(high, seq)
                self.counts["examined"] += 1
            last = reconcile[-1][2] if len(reconcile) == self.page_size else 0
            self.store.db.execute("INSERT OR REPLACE INTO device_import_cursor VALUES (?,?,?,?,?,?)", (*key,last,high))
            return bool(tail or reconcile)

    def coverage(self):
        rows = self.store.db.execute("SELECT mac,ring,scan_seq,high_seq FROM device_import_cursor WHERE site=? AND filename=? ORDER BY mac,ring LIMIT 33", (self.site_id,self.filename)).fetchall()
        return {"cursors": [dict(zip(("mac","ring","scan_seq","highest_imported_seq"),row)) for row in rows[:32]], "truncated": len(rows)>32, **self.counts}
