"""Single-owner SQLite journal. An ACK is safe only after save() returns."""
from datetime import datetime, timezone
from pathlib import Path
import sqlite3


class IdentityCollision(ValueError):
    pass


class Store:
    def __init__(self, filename):
        self.path = Path(filename).resolve()
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(self.path, timeout=2, isolation_level=None)
        try:
            self.db.execute("PRAGMA busy_timeout=2000")
            mode = self.db.execute("PRAGMA journal_mode=WAL").fetchone()[0]
            self.db.execute("PRAGMA synchronous=FULL")
            if mode.lower() != "wal" or self.db.execute("PRAGMA synchronous").fetchone()[0] != 2:
                raise sqlite3.DatabaseError("required WAL/FULL durability unavailable")
            if self.db.execute("PRAGMA quick_check").fetchall() != [("ok",)]:
                raise sqlite3.DatabaseError("database integrity check failed")
            version = self.db.execute("PRAGMA user_version").fetchone()[0]
            if version not in (0, 1, 2):
                raise sqlite3.DatabaseError("unsupported database schema version")
            self.db.executescript("""
                BEGIN IMMEDIATE;
                CREATE TABLE IF NOT EXISTS records (
                    mac TEXT NOT NULL, ring TEXT NOT NULL, seq INTEGER NOT NULL,
                    device TEXT NOT NULL, raw BLOB NOT NULL CHECK(length(raw)=64),
                    received_utc TEXT NOT NULL,
                    PRIMARY KEY(mac, ring, seq)
                ) WITHOUT ROWID;
                PRAGMA user_version=1;
                COMMIT;
            """)
            columns = {row[1] for row in self.db.execute("PRAGMA table_info(records)")}
            self.db.execute("BEGIN IMMEDIATE")
            for column, kind in (("arrival_utc", "TEXT"), ("persisted_utc", "TEXT"),
                                 ("queue_latency_ms", "REAL")):
                if column not in columns:
                    self.db.execute("ALTER TABLE records ADD COLUMN " + column + " " + kind)
            self.db.execute("PRAGMA user_version=2")
            self.db.execute("COMMIT")
        except Exception:
            self.db.close()
            raise

    def save(self, batch, boundary_hook=None, *, arrival_utc=None, queue_latency_ms=None):
        """Commit complete batch or nothing; boundary_hook is for crash tests only."""
        now = datetime.now(timezone.utc).isoformat()
        self.db.execute("BEGIN IMMEDIATE")
        inserted = []
        try:
            for record in batch.records:
                previous = self.db.execute(
                    "SELECT device, raw FROM records WHERE mac=? AND ring=? AND seq=?",
                    (batch.mac, batch.ring, record.seq),
                ).fetchone()
                if previous is not None:
                    if previous != (batch.device, record.raw):
                        raise IdentityCollision("different bytes/device at existing mac/ring/seq")
                    continue
                self.db.execute("INSERT INTO records (mac,ring,seq,device,raw,received_utc,arrival_utc,persisted_utc,queue_latency_ms) VALUES (?,?,?,?,?,?,?,?,?)",
                                (batch.mac, batch.ring, record.seq, batch.device, record.raw, now,
                                 arrival_utc, None, queue_latency_ms))
                inserted.append(record.seq)
            if boundary_hook:
                boundary_hook("before_commit")
            # Durable timestamp denotes COMMIT request, not completion. Exact
            # completion cannot be written inside the transaction it times.
            committing = datetime.now(timezone.utc).isoformat()
            self.db.executemany("UPDATE records SET persisted_utc=? WHERE mac=? AND ring=? AND seq=?",
                                ((committing,batch.mac,batch.ring,seq) for seq in inserted))
            self.db.execute("COMMIT")
            if boundary_hook:
                boundary_hook("after_commit")
        except Exception:
            if self.db.in_transaction:
                self.db.execute("ROLLBACK")
            raise

    def close(self):
        self.db.close()
