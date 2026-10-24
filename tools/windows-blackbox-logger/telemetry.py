"""Cross-source journal with bounded ingestion/export, separate from device ACKs.

UTC normalization says nothing about clock accuracy. A timestamp is only useful
for certified interval overlap if its clock quality AND uncertainty are known.
"""
from dataclasses import asdict, dataclass, field
from datetime import datetime, timedelta, timezone
import base64
import json
import math
from pathlib import Path
import re
import sqlite3

MAX_RAW_BYTES = 16384
MAX_FIELDS_BYTES = 16384
MAX_EVENT_BYTES = 40960
MAX_ROWS = 100000
TRUSTED_CLOCKS = frozenset(("measured", "synchronized", "anchored"))


def utc_now():
    return datetime.now(timezone.utc).isoformat(timespec="microseconds")


def parse_utc(value):
    if not isinstance(value, str) or len(value) > 64:
        raise ValueError("timestamp must be an ISO 8601 string with explicit offset")
    try:
        result = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise ValueError("invalid ISO 8601 timestamp") from exc
    if result.tzinfo is None or result.utcoffset() is None:
        raise ValueError("timestamp needs an explicit UTC offset; local time is ambiguous")
    return result.astimezone(timezone.utc)


def normalize_utc(value):
    return parse_utc(value).isoformat(timespec="microseconds")


def canonical_json(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
                      allow_nan=False)


def bounded_text(value, label, maximum, allow_empty=False):
    if not isinstance(value, str) or len(value.encode("utf-8")) > maximum:
        raise ValueError(label + " must be bounded text")
    if not allow_empty and not value.strip():
        raise ValueError(label + " must not be empty")
    if "\x00" in value:
        raise ValueError(label + " must not contain NUL")
    return value


def normalize_mac(value):
    if value is None:
        return None
    if not isinstance(value, str):
        raise ValueError("MAC must be text")
    normalized = value.replace(":", "").replace("-", "").upper()
    if not re.fullmatch(r"[0-9A-F]{12}", normalized):
        raise ValueError("invalid MAC")
    return normalized


def bounded_limit(limit):
    if isinstance(limit, bool) or not isinstance(limit, int) or not 1 <= limit <= MAX_ROWS:
        raise ValueError("limit must be 1..100000")
    return limit


@dataclass(frozen=True)
class Event:
    site_id: str
    source: str
    source_id: str
    event_id: str
    observed_utc: str
    occurred_utc: str = None
    clock_quality: str = "unverified"
    uncertainty_ms: float = None
    mac: str = None
    device_id: str = None
    event_type: str = "unknown"
    fields: dict = field(default_factory=dict)
    raw: str = ""

    def __post_init__(self):
        for name, maximum in (("site_id", 128), ("source", 64), ("source_id", 256),
                              ("event_id", 256), ("clock_quality", 64), ("event_type", 128)):
            bounded_text(getattr(self, name), name, maximum)
        if self.device_id is not None:
            bounded_text(self.device_id, "device_id", 256)
        object.__setattr__(self, "mac", normalize_mac(self.mac))
        object.__setattr__(self, "observed_utc", normalize_utc(self.observed_utc))
        if self.occurred_utc is not None:
            object.__setattr__(self, "occurred_utc", normalize_utc(self.occurred_utc))
        if self.uncertainty_ms is not None:
            if (isinstance(self.uncertainty_ms, bool) or
                    not isinstance(self.uncertainty_ms, (int, float)) or
                    not math.isfinite(self.uncertainty_ms) or
                    not 0 <= self.uncertainty_ms <= 86400000):
                raise ValueError("uncertainty_ms must be finite and between 0 and one day")
        if not isinstance(self.fields, dict):
            raise ValueError("fields must be an object")
        if len(canonical_json(self.fields).encode("utf-8")) > MAX_FIELDS_BYTES:
            raise ValueError("fields exceed bounded size")
        bounded_text(self.raw, "raw", MAX_RAW_BYTES, allow_empty=True)
        if len(canonical_json(asdict(self)).encode("utf-8")) > MAX_EVENT_BYTES:
            raise ValueError("event exceeds bounded size")

    @classmethod
    def from_dict(cls, value, observed_utc=None):
        if not isinstance(value, dict):
            raise ValueError("event must be an object")
        data = dict(value)
        version = data.pop("schema_version", 1)
        if type(version) is not int or version != 1:
            raise ValueError("unsupported event schema")
        if observed_utc is not None:
            data["observed_utc"] = observed_utc
        try:
            return cls(**data)
        except TypeError as exc:
            raise ValueError("unknown or missing event field") from exc

    def to_dict(self):
        return {"schema_version": 1, **asdict(self)}


class TelemetryConflict(ValueError):
    """Same source identity with different facts: never overwrite silently."""


class TelemetryStore:
    def __init__(self, filename):
        self.path = Path(filename).resolve()
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(self.path, timeout=2, isolation_level=None)
        try:
            self.db.execute("PRAGMA busy_timeout=2000")
            # Identify before changing journal settings: an accidental path to
            # the device ACK database must not reconfigure that database.
            # One SQLite read snapshot avoids app_id=0 from before another
            # initializer commits being combined with its new tables afterward.
            application, table_count = self.db.execute(
                "SELECT application_id,(SELECT count(*) FROM sqlite_master WHERE type='table') FROM pragma_application_id"
            ).fetchone()
            if application not in (0, 0x48425431) or (application == 0 and table_count):
                raise sqlite3.DatabaseError("not a telemetry database")
            mode = self.db.execute("PRAGMA journal_mode=WAL").fetchone()[0]
            self.db.execute("PRAGMA synchronous=FULL")
            if mode.lower() != "wal" or self.db.execute("PRAGMA synchronous").fetchone()[0] != 2:
                raise sqlite3.DatabaseError("required WAL/FULL durability unavailable")
            version = self.db.execute("PRAGMA user_version").fetchone()[0]
            if version not in (0, 1, 2):
                raise sqlite3.DatabaseError("unsupported telemetry schema")
            # A distinct application_id prevents accidentally treating device ACK
            # storage (or another application's database) as this journal.
            self.db.executescript("""
                BEGIN IMMEDIATE;
                CREATE TABLE IF NOT EXISTS telemetry_events (
                  site_id TEXT NOT NULL, source TEXT NOT NULL, source_id TEXT NOT NULL,
                  event_id TEXT NOT NULL, occurred_utc TEXT, observed_utc TEXT NOT NULL,
                  payload TEXT NOT NULL, facts TEXT NOT NULL,
                  PRIMARY KEY(site_id,source,source_id,event_id)
                ) WITHOUT ROWID;
                CREATE INDEX IF NOT EXISTS telemetry_time ON telemetry_events(site_id,occurred_utc);
                CREATE TABLE IF NOT EXISTS event_times (
                  site_id TEXT NOT NULL, source TEXT NOT NULL, source_id TEXT NOT NULL,
                  event_id TEXT NOT NULL, occurred_utc TEXT, observed_utc TEXT NOT NULL,
                  utc_min TEXT, utc_max TEXT, mac TEXT, ring TEXT,
                  boot INTEGER, seq INTEGER, uptime INTEGER,
                  PRIMARY KEY(site_id,source,source_id,event_id)
                ) WITHOUT ROWID;
                CREATE INDEX IF NOT EXISTS event_times_window ON event_times(site_id,utc_min,utc_max);
                CREATE INDEX IF NOT EXISTS event_times_device ON event_times(site_id,mac,ring,boot,seq,uptime);
                CREATE INDEX IF NOT EXISTS event_times_order ON event_times(site_id,occurred_utc,source,source_id,event_id);
                CREATE INDEX IF NOT EXISTS event_times_observed ON event_times(site_id,observed_utc,source,source_id,event_id);
                CREATE TABLE IF NOT EXISTS clock_anchors (
                  site_id TEXT NOT NULL, anchor_id TEXT NOT NULL, payload TEXT NOT NULL,
                  PRIMARY KEY(site_id,anchor_id)
                ) WITHOUT ROWID;
                PRAGMA application_id=1212306481;
                COMMIT;
            """)
            # The initial telemetry schema had only source payloads. Rebuild the
            # derived search index on upgrade, without rewriting original facts.
            if version in (0, 1):
                self.db.execute("BEGIN IMMEDIATE")
                try:
                    cursor = self.db.execute("SELECT payload FROM telemetry_events")
                    while True:
                        rows = cursor.fetchmany(128)
                        if not rows:
                            break
                        for row in rows:
                            self._index_event(Event.from_dict(json.loads(row[0])))
                    self.db.execute("PRAGMA user_version=2")
                    self.db.execute("COMMIT")
                except Exception:
                    self.db.execute("ROLLBACK")
                    raise
        except Exception:
            self.db.close()
            raise

    def _index_event(self, event):
        occurred = None if event.source == "device" else event.occurred_utc
        lower = upper = occurred
        if occurred is not None and event.clock_quality in TRUSTED_CLOCKS and event.uncertainty_ms is not None:
            center = parse_utc(occurred)
            delta = timedelta(milliseconds=event.uncertainty_ms)
            lower, upper = ((center - delta).isoformat(timespec="microseconds"),
                            (center + delta).isoformat(timespec="microseconds"))
        fields = event.fields if event.source == "device" else {}
        def number(name):
            value = fields.get(name)
            return value if type(value) is int and 0 <= value < (1 << 32) else None
        ring = fields.get("ring") if isinstance(fields.get("ring"), str) else None
        self.db.execute("INSERT OR REPLACE INTO event_times VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)",
                        (event.site_id, event.source, event.source_id, event.event_id, occurred,
                         event.observed_utc, lower, upper, event.mac, ring,
                         number("boot"), number("seq"), number("uptime_ms")))

    def save(self, event):
        if not isinstance(event, Event):
            event = Event.from_dict(event)
        payload = event.to_dict()
        facts = dict(payload)
        # Repeated exports/UDP relay arrival time is not a new source fact.
        facts.pop("observed_utc")
        stable = canonical_json(facts)
        key = (event.site_id, event.source, event.source_id, event.event_id)
        self.db.execute("BEGIN IMMEDIATE")
        try:
            previous = self.db.execute(
                "SELECT facts FROM telemetry_events WHERE site_id=? AND source=? AND source_id=? AND event_id=?", key
            ).fetchone()
            if previous is not None:
                if previous[0] != stable:
                    raise TelemetryConflict("event identity has different facts")
                self.db.execute("COMMIT")
                return False
            self.db.execute("INSERT INTO telemetry_events VALUES (?,?,?,?,?,?,?,?)",
                            (*key, event.occurred_utc, event.observed_utc,
                             canonical_json(payload), stable))
            self._index_event(event)
            self.db.execute("COMMIT")
            return True
        except Exception:
            if self.db.in_transaction:
                self.db.execute("ROLLBACK")
            raise

    def events(self, site_id, limit=10000):
        bounded_limit(limit)
        bounded_text(site_id, "site_id", 128)
        rows = self.db.execute(
            "SELECT payload FROM telemetry_events WHERE site_id=? "
            "ORDER BY occurred_utc IS NULL,occurred_utc,observed_utc,source,source_id,event_id LIMIT ?",
            (site_id, limit))
        for row in rows:
            yield Event.from_dict(json.loads(row[0]))

    def event_count(self, site_id):
        return self.db.execute("SELECT count(*) FROM telemetry_events WHERE site_id=?", (site_id,)).fetchone()[0]

    def prepare_timeline_criteria(self, criteria, all_criteria=None):
        """Temporary, bounded anchor ranges used only for report selection."""
        if all_criteria is None:
            all_criteria = criteria
        if len(criteria) > 1000 or len(all_criteria) > 1000:
            raise ValueError("too many anchor ranges")
        for name, values in (("timeline_criteria", criteria), ("timeline_all_criteria", all_criteria)):
            self.db.execute("CREATE TEMP TABLE IF NOT EXISTS " + name + "(mac TEXT,ring TEXT,boot INTEGER,seq_min INTEGER,seq_max INTEGER,uptime_min INTEGER,uptime_max INTEGER)")
            self.db.execute("DELETE FROM " + name)
            self.db.executemany("INSERT INTO " + name + " VALUES (?,?,?,?,?,?,?)", values)

    def prepare_anchor_query(self, anchors):
        """Stream all validated anchors into an indexed, report-local SQLite table.

        No global anchor count cap and no Python all-pairs comparison. Existing
        journals with many boots remain queryable; only selected events are
        materialized. This work is outside the MQTT ACK path.
        """
        def shift_us(delta_ms, drift_ppm, direction):
            # Match datetime.timedelta's microsecond rounding exactly. Float
            # epoch arithmetic can otherwise drop a boundary event or invent
            # an anchor conflict by a fraction of a microsecond.
            delta = timedelta(milliseconds=delta_ms + direction * abs(delta_ms) * drift_ppm / 1000000)
            return (delta.days * 86400 + delta.seconds) * 1000000 + delta.microseconds
        self.db.create_function("bb_shift_us", 3, shift_us, deterministic=True)
        self.db.execute("CREATE TEMP TABLE IF NOT EXISTS timeline_anchor_ranges (mac TEXT,ring TEXT,boot INTEGER,seq_min INTEGER,seq_max INTEGER,uptime_min INTEGER,uptime_max INTEGER,sample_uptime INTEGER,utc_min_us INTEGER,utc_max_us INTEGER,drift REAL,payload TEXT)")
        self.db.execute("CREATE INDEX IF NOT EXISTS temp.timeline_anchor_identity ON timeline_anchor_ranges(mac,ring,boot,uptime_min,uptime_max)")
        self.db.execute("DELETE FROM timeline_anchor_ranges")
        epoch = datetime(1970, 1, 1, tzinfo=timezone.utc)
        def micros(value):
            delta = parse_utc(value) - epoch
            return (delta.days * 86400 + delta.seconds) * 1000000 + delta.microseconds
        def rows():
            for anchor in anchors:
                selected = anchor.selection_range()
                if selected is not None:
                    yield (*selected, anchor.uptime_ms64 - anchor.record_uptime_wraps * (1 << 32),
                           micros(anchor.utc_min), micros(anchor.utc_max), anchor.drift_ppm,
                           canonical_json(anchor.to_dict()))
        self.db.executemany("INSERT INTO timeline_anchor_ranges VALUES (?,?,?,?,?,?,?,?,?,?,?,?)", rows())

    def matching_anchor_dicts(self, event):
        if event.source != "device":
            return
        f = event.fields
        rows = self.db.execute("SELECT payload FROM timeline_anchor_ranges WHERE mac=? AND ring=? AND boot=? AND ? BETWEEN seq_min AND seq_max AND ? BETWEEN uptime_min AND uptime_max",
                               (event.mac, f.get("ring"), f.get("boot"), f.get("seq"), f.get("uptime_ms")))
        for row in rows:
            yield json.loads(row[0])

    def iter_anchor_dicts(self, site_id):
        for row in self.db.execute("SELECT payload FROM clock_anchors WHERE site_id=? ORDER BY anchor_id", (site_id,)):
            yield json.loads(row[0])

    def timeline_page(self, site_id, section, limit=10000, window=None, cursor=None):
        """Apply time/anchor selection BEFORE independent per-section limits.

        Cursors are stable order bookmarks, not snapshot tokens. Use the same
        site/window/anchors while paging; newly ingested earlier events require a
        fresh report. A bounded lookahead determines whether another page exists.
        """
        bounded_limit(limit)
        if section not in ("timestamped", "device_candidates", "unaligned"):
            raise ValueError("unknown timeline selection")
        criteria = ("EXISTS(SELECT 1 FROM timeline_anchor_ranges c WHERE t.source='device' "
                    "AND c.mac=t.mac AND c.ring=t.ring AND c.boot=t.boot "
                    "AND t.seq BETWEEN c.seq_min AND c.seq_max AND t.uptime BETWEEN c.uptime_min AND c.uptime_max)")
        conditions, parameters = ["t.site_id=?"], [site_id]
        time_column = "occurred_utc" if section == "timestamped" else "observed_utc"
        if section == "timestamped":
            conditions.append("t.occurred_utc IS NOT NULL")
            if window:
                conditions.extend(("t.utc_min<=?", "t.utc_max>=?"))
                parameters.extend((window[1], window[0]))
        elif section == "device_candidates":
            conditions.append(criteria)
        else:
            conditions.extend(("t.occurred_utc IS NULL", "NOT " + criteria))
        columns = "t." + time_column + ",t.source,t.source_id,t.event_id"
        if cursor:
            try:
                if not isinstance(cursor, str) or len(cursor) > 2048:
                    raise ValueError("cursor length")
                decoded = json.loads(base64.urlsafe_b64decode(cursor.encode("ascii")).decode("utf-8"))
                if (decoded.get("site_id") != site_id or decoded.get("section") != section or
                        decoded.get("window") != (list(window) if window else None) or
                        not isinstance(decoded.get("key"), list) or len(decoded["key"]) != 4 or
                        any(not isinstance(part, str) for part in decoded["key"])):
                    raise ValueError("cursor context")
            except Exception as exc:
                raise ValueError("invalid or mismatched timeline cursor") from exc
            conditions.append("(" + columns + ")>(?,?,?,?)")
            parameters.extend(decoded["key"])
        if section == "device_candidates" and window:
            # Intersect applicable measurement intervals for each event before
            # limiting. A conflict remains unknown even outside every nominal
            # window; healthy old anchors cannot hide a newer incident page.
            join = ("c.mac=t.mac AND c.ring=t.ring AND c.boot=t.boot AND "
                    "t.seq BETWEEN c.seq_min AND c.seq_max AND t.uptime BETWEEN c.uptime_min AND c.uptime_max")
            lower = "MAX(c.utc_min_us+bb_shift_us(t.uptime-c.sample_uptime,c.drift,-1))"
            upper = "MIN(c.utc_max_us+bb_shift_us(t.uptime-c.sample_uptime,c.drift,1))"
            epoch = datetime(1970, 1, 1, tzinfo=timezone.utc)
            def micros(value):
                delta = parse_utc(value) - epoch
                return (delta.days * 86400 + delta.seconds) * 1000000 + delta.microseconds
            query = ("SELECT e.payload," + columns + " FROM event_times t JOIN telemetry_events e "
                     "USING(site_id,source,source_id,event_id) JOIN timeline_anchor_ranges c ON " + join +
                     " WHERE " + " AND ".join(conditions) +
                     " GROUP BY t.site_id,t.source,t.source_id,t.event_id HAVING (" + lower + "<=? AND " + upper + ">=?) OR " + lower + ">" + upper +
                     " ORDER BY " + columns + " LIMIT ?")
            parameters.extend((micros(window[1]), micros(window[0])))
        else:
            query = ("SELECT e.payload," + columns + " FROM event_times t JOIN telemetry_events e "
                     "USING(site_id,source,source_id,event_id) WHERE " + " AND ".join(conditions) +
                     " ORDER BY " + columns + " LIMIT ?")
        rows = self.db.execute(query, (*parameters, limit + 1)).fetchall()
        more = len(rows) > limit
        selected = rows[:limit]
        bookmark = None
        if more and selected:
            bookmark = base64.urlsafe_b64encode(canonical_json({"site_id": site_id, "section": section,
                "window": list(window) if window else None, "key": list(selected[-1][1:])}).encode("utf-8")).decode("ascii")
        return [Event.from_dict(json.loads(row[0])) for row in selected], more, bookmark

    def save_anchor(self, anchor):
        # Anchor validates the complete mapping before it reaches this method.
        payload = canonical_json(anchor.to_dict())
        self.db.execute("BEGIN IMMEDIATE")
        try:
            row = self.db.execute("SELECT payload FROM clock_anchors WHERE site_id=? AND anchor_id=?",
                                  (anchor.site_id, anchor.anchor_id)).fetchone()
            if row is not None:
                previous = json.loads(row[0])
                previous.setdefault("clock_quality", "measured")
                if canonical_json(previous) != payload:
                    raise TelemetryConflict("anchor identity has different facts")
            if row is None:
                self.db.execute("INSERT INTO clock_anchors VALUES (?,?,?)", (anchor.site_id, anchor.anchor_id, payload))
            self.db.execute("COMMIT")
            return row is None
        except Exception:
            if self.db.in_transaction:
                self.db.execute("ROLLBACK")
            raise

    def anchor_dicts(self, limit=1000, site_id=None, window=None):
        bounded_limit(limit)
        conditions, params = [], []
        if site_id:
            conditions.append("site_id=?")
            params.append(site_id)
        if window:
            # A measured sample can cover events up to max_delta away. Use the
            # maximum allowed drift expansion (10%) for conservative selection.
            conditions.append("julianday(json_extract(payload,'$.utc_min'))-1.1*coalesce(json_extract(payload,'$.max_delta_ms'),600000)/86400000.0 <= julianday(?) AND julianday(json_extract(payload,'$.utc_max'))+1.1*coalesce(json_extract(payload,'$.max_delta_ms'),600000)/86400000.0 >= julianday(?)")
            params.extend((window[1],window[0]))
        query = "SELECT payload FROM clock_anchors"
        if conditions:
            query += " WHERE "+" AND ".join(conditions)
        rows = self.db.execute(query+" ORDER BY anchor_id LIMIT ?", (*params,limit+1)).fetchall()
        if len(rows) > limit:
            raise ValueError("anchor limit exceeded; narrow the journal")
        return [json.loads(row[0]) for row in rows]

    def close(self):
        self.db.close()
