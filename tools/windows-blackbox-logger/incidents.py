"""Derived game-aware incidents. Never participates in device durable ACKs.

Only fresh, complete server snapshots authorize notifications. MQTT silence and
power cycles are evidence, never an offline rule. Raw telemetry is unchanged.
"""
import argparse
from contextlib import closing
from datetime import timedelta
import json
import logging
from pathlib import Path
import sqlite3
import threading
import time
import uuid

from telemetry import Event, canonical_json, normalize_mac, parse_utc, utc_now

LOG = logging.getLogger("blackbox")
APP_ID = 0x48424931
FRESH_SECONDS = 30
BATCH = 128


def validate_config(value, base_path, telemetry_database, device_database, server):
    if not isinstance(value, dict) or set(value) - {"enabled", "database", "notifications"}:
        raise ValueError("incidents contains unknown fields")
    enabled = value.get("enabled", False)
    if type(enabled) is not bool:
        raise ValueError("incidents enabled must be boolean")
    name = value.get("database", "incidents.sqlite3")
    if not isinstance(name, str) or not name.strip() or len(name) > 1024:
        raise ValueError("incidents database requires a path")
    database = str((Path(base_path) / name).resolve())
    if Path(database) in {Path(telemetry_database).resolve(), Path(device_database).resolve()}:
        raise ValueError("incidents database must be separate from raw databases")
    if enabled and not server.get("enabled"):
        raise ValueError("incidents requires enabled server_jsonl")
    from notifications import validate_config as validate_notifications
    settings = validate_notifications(value.get("notifications", {"enabled": False}), base_path)
    return {"enabled": enabled, "database": database, "notifications": settings}


def _text(value):
    return value if isinstance(value, str) and 0 < len(value) <= 256 and "\x00" not in value else None


def _fresh(event, now):
    if not event.occurred_utc:
        return False
    # Rereading old files updates observation but cannot rejuvenate occurrence.
    return all(-2 <= (parse_utc(now) - parse_utc(stamp)).total_seconds() <= FRESH_SECONDS
               for stamp in (event.occurred_utc, event.observed_utc))


class IncidentStore:
    def __init__(self, path, *, readonly=False):
        self.path = Path(path).resolve()
        if not readonly:
            self.path.parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(self.path.as_uri()+"?mode=ro" if readonly else self.path, uri=readonly, timeout=2, isolation_level=None)
        self.db.row_factory = sqlite3.Row
        try:
            if readonly:
                if self.db.execute("PRAGMA application_id").fetchone()[0] != APP_ID or self.db.execute("PRAGMA user_version").fetchone()[0] != 1:
                    raise sqlite3.DatabaseError("not a supported incidents database")
                self.db.execute("PRAGMA query_only=ON")
            else:
                self._initialize()
        except Exception:
            self.db.close()
            raise

    def _initialize(self):
        app, count = self.db.execute("SELECT application_id,(SELECT count(*) FROM sqlite_master WHERE type='table') FROM pragma_application_id").fetchone()
        if app not in (0, APP_ID) or (app == 0 and count):
            self.db.close()
            raise sqlite3.DatabaseError("not an incidents database")
        if self.db.execute("PRAGMA user_version").fetchone()[0] not in (0, 1):
            self.db.close()
            raise sqlite3.DatabaseError("unsupported incidents database")
        mode = self.db.execute("PRAGMA journal_mode=WAL").fetchone()[0]
        self.db.execute("PRAGMA synchronous=FULL")
        if mode.lower() != "wal" or self.db.execute("PRAGMA synchronous").fetchone()[0] != 2:
            raise sqlite3.DatabaseError("required incident WAL/FULL durability unavailable")
        self.db.executescript("""
          BEGIN IMMEDIATE;
          CREATE TABLE IF NOT EXISTS incident_state (
            incident_id TEXT PRIMARY KEY, site_id TEXT NOT NULL, source_id TEXT NOT NULL,
            boot_id TEXT NOT NULL, session_id TEXT NOT NULL, device_id TEXT, mac TEXT,
            state TEXT NOT NULL, reason TEXT, opened_utc TEXT NOT NULL, updated_utc TEXT NOT NULL,
            details TEXT NOT NULL
          );
          CREATE INDEX IF NOT EXISTS incident_source ON incident_state(site_id,source_id,state);
          CREATE TABLE IF NOT EXISTS incident_outbox (
            id INTEGER PRIMARY KEY AUTOINCREMENT, incident_id TEXT NOT NULL,
            destination TEXT NOT NULL, version INTEGER NOT NULL, payload TEXT NOT NULL,
            state TEXT NOT NULL DEFAULT 'pending', attempts INTEGER NOT NULL DEFAULT 0,
            next_attempt REAL NOT NULL DEFAULT 0, error_code TEXT, remote_id TEXT,
            UNIQUE(incident_id,destination,version)
          );
          CREATE INDEX IF NOT EXISTS incident_pending ON incident_outbox(state,next_attempt);
          CREATE TABLE IF NOT EXISTS incident_sources (
            site_id TEXT NOT NULL,source_id TEXT NOT NULL,boot_id TEXT,session_id TEXT,
            snapshot_utc TEXT, event_seq INTEGER, validated_run TEXT, deadline REAL,
            active_ids TEXT NOT NULL DEFAULT '[]', health TEXT NOT NULL DEFAULT 'unknown',
            PRIMARY KEY(site_id,source_id)
          );
          CREATE TABLE IF NOT EXISTS incident_meta (key TEXT PRIMARY KEY,value TEXT NOT NULL);
          PRAGMA application_id=1212303665;
          PRAGMA user_version=1;
          COMMIT;
        """)

    def close(self):
        self.db.close()

    def cursor(self):
        row = self.db.execute("SELECT value FROM incident_meta WHERE key='cursor'").fetchone()
        return int(row[0]) if row else 0

    def start_run(self, run_id):
        # A previous process's monotonic lease is never valid in this process.
        with self.transaction():
            self.db.execute("UPDATE incident_sources SET validated_run=NULL,health='unknown'")
            self.db.execute("UPDATE incident_outbox SET state='manual_review',error_code='interrupted_delivery' WHERE state='sending'")
            self.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('run_id',?)", (run_id,))
            self.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('delivery_run','')")

    def transaction(self):
        class Transaction:
            def __enter__(inner):
                self.db.execute("BEGIN IMMEDIATE")
            def __exit__(inner, kind, value, tb):
                self.db.execute("ROLLBACK" if kind else "COMMIT")
        return Transaction()

    def invalidate(self, reason="server_stale"):
        self.db.execute("UPDATE incident_sources SET validated_run=NULL,health=?", (reason,))

    def expire(self, run_id, mono):
        self.db.execute("UPDATE incident_sources SET validated_run=NULL,health='server_stale' WHERE validated_run=? AND deadline<?", (run_id, mono))

    def _queue(self, row, channels, version, status):
        details = json.loads(row["details"])
        role = _text(details.get("role")) or "unknown"
        reason = row["reason"] or "unknown"
        payload = {"incident_id": row["incident_id"], "site_id": row["site_id"],
                   "device_mac": row["mac"], "device_id": row["device_id"],
                   "game_session_id": row["session_id"], "status": status,
                   "summary": f"Device {row['device_id']} / role {role} / {status} / server reason {reason}; last_seen={details.get('last_seen_utc', 'unknown')}; gap_ms={details.get('gap_ms', 'unknown')}. Cause not determined.",
                   "occurred_at": row["opened_utc"] if status == "offline" else row["updated_utc"],
                   # Structured copies of the summary values for the human-readable Slack card.
                   "role": role, "server_reason": details.get("reason"), "close_reason": row["reason"],
                   "opened_at": row["opened_utc"], "last_seen_utc": details.get("last_seen_utc"),
                   "gap_ms": details.get("gap_ms")}
        for channel in channels:
            linked = {}
            if version > 1:
                delivered = self.db.execute("SELECT remote_id FROM incident_outbox WHERE incident_id=? AND destination=? AND version=1 AND state='delivered'", (row["incident_id"], channel)).fetchone()
                # A bot-posted Slack card has a message ts: its follow-up edits that card and
                # threads under it, so even monitoring_ended closes it out. Webhooks only post recoveries.
                if channel == "slack" and delivered and delivered[0]:
                    linked = {"slack_ts": delivered[0]}
                if channel == "notion" and delivered and delivered[0]:
                    linked = {"notion_page_id": delivered[0]}
                if not delivered or (status != "recovered" and channel != "notion" and not linked):
                    continue
            self.db.execute("INSERT OR IGNORE INTO incident_outbox(incident_id,destination,version,payload) VALUES (?,?,?,?)", (row["incident_id"], channel, version, canonical_json(dict(payload, **linked))))

    def _close(self, incident_id, state, reason, when, channels):
        previous = self.db.execute("SELECT state FROM incident_state WHERE incident_id=?", (incident_id,)).fetchone()
        if not previous or previous[0] != "open":
            return
        self.db.execute("UPDATE incident_state SET state=?,reason=?,updated_utc=? WHERE incident_id=? AND state='open'", (state, reason, when, incident_id))
        self.db.execute("UPDATE incident_outbox SET state='canceled',error_code='incident_closed_before_delivery' WHERE incident_id=? AND version=1 AND state IN ('pending','retryable')", (incident_id,))
        row = self.db.execute("SELECT * FROM incident_state WHERE incident_id=?", (incident_id,)).fetchone()
        if row:
            self._queue(row, channels, 2, "recovered" if state == "recovered" else "monitoring_ended")

    def _record_open(self, event, item, fields, *, snapshot=False):
        incident = _text(item.get("incident_id"))
        boot, session = _text(fields.get("boot_id")), _text(fields.get("game_session_id"))
        device = _text(item.get("device_id")) or event.device_id
        if not incident or not boot or not session or not device:
            return
        mac = item.get("mac") if snapshot else event.mac
        try:
            mac = normalize_mac(mac)
        except ValueError:
            mac = None  # Never guess MAC from display names.
        opened = item.get("opened_utc") or event.occurred_utc
        try:
            opened = parse_utc(opened).isoformat(timespec="microseconds")
        except (ValueError, TypeError):
            return
        details = {key:fields[key] for key in ("boot_id","game_session_id","game_group","role","reason","last_seen_utc","gap_ms","request_id","request_scope") if key in fields}
        for key in ("role","last_seen_utc"):
            if key in item:
                details[key] = item[key]
        self.db.execute("INSERT OR IGNORE INTO incident_state VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                        (incident, event.site_id, event.source_id, boot, session, device, mac,
                         "open", "server_device_offline", opened, event.occurred_utc, canonical_json(details)))
        if snapshot:
            # A lost/ambiguous inventory binding must remove correlation by MAC,
            # rather than retain a previously plausible but now stale identity.
            self.db.execute("UPDATE incident_state SET mac=?,updated_utc=? WHERE incident_id=? AND site_id=? AND source_id=? AND boot_id=? AND session_id=? AND state='open'", (mac,event.occurred_utc,incident,event.site_id,event.source_id,boot,session))
            row = self.db.execute("SELECT * FROM incident_state WHERE incident_id=?", (incident,)).fetchone()
            if row:
                for pending in self.db.execute("SELECT id,payload FROM incident_outbox WHERE incident_id=? AND state IN ('pending','retryable')",(incident,)):
                    payload = json.loads(pending["payload"])
                    payload["device_mac"] = mac
                    self.db.execute("UPDATE incident_outbox SET payload=? WHERE id=?",(canonical_json(payload),pending["id"]))

    def process(self, seq, event, *, now, mono, run_id, channels):
        with self.transaction():
            floor = self.db.execute("SELECT value FROM incident_meta WHERE key='snapshot_floor'").fetchone()
            if event.event_type != "game_monitor_snapshot" or not floor or seq > int(floor[0]):
                self._process(event, now=now, mono=mono, run_id=run_id, channels=channels)
            self.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('cursor',?)", (str(seq),))

    def _process(self, event, *, now, mono, run_id, channels):
        if event.source != "game_server":
            return
        fields = event.fields
        boot, session = _text(fields.get("boot_id")), _text(fields.get("game_session_id"))
        incident = _text(fields.get("incident_id"))
        if event.event_type == "device_offline":
            self._record_open(event, {"incident_id": incident}, fields)
            return  # No notification until a complete fresh snapshot confirms it.
        if event.event_type in ("device_recovered", "alarm_cleared"):
            if not incident or not boot or not session or not event.occurred_utc:
                return
            row = self.db.execute("SELECT * FROM incident_state WHERE incident_id=? AND site_id=? AND source_id=? AND boot_id=? AND session_id=?", (incident,event.site_id,event.source_id,boot,session)).fetchone()
            if row:
                recovered = event.event_type == "device_recovered" and fields.get("reason") == "reconnected" and _fresh(event, now)
                self._close(incident, "recovered" if recovered else "closed", _text(fields.get("reason")) or "unknown", event.occurred_utc, channels)
            return
        if event.event_type != "game_monitor_snapshot" or not boot or not _fresh(event, now):
            return
        sequence = fields.get("event_seq")
        if type(sequence) is not int or sequence < 0:
            return
        previous = self.db.execute("SELECT * FROM incident_sources WHERE site_id=? AND source_id=?", (event.site_id,event.source_id)).fetchone()
        if previous and (previous["snapshot_utc"] > event.occurred_utc or (previous["boot_id"] == boot and previous["event_seq"] >= sequence)):
            return
        # A newer incomplete snapshot revokes the older lease immediately.
        self.db.execute("INSERT OR REPLACE INTO incident_sources VALUES (?,?,?,?,?,?,?,?,?,?)", (event.site_id,event.source_id,boot,session,event.occurred_utc,sequence,None,None,"[]","invalid_snapshot"))
        # Oversized, partial or malformed snapshots fail closed: no inferred
        # recovery and no alarms from incomplete device inventory.
        active, participants = fields.get("active_incidents"), fields.get("participants")
        if fields.get("snapshot_complete") is not True or type(fields.get("game_active")) is not bool or not isinstance(active, list) or len(active) > 64 or not isinstance(participants, list) or len(participants) > 64:
            return
        if any(not isinstance(item, dict) or not _text(item.get("incident_id")) or not _text(item.get("device_id")) for item in active):
            return
        if any(not isinstance(item, dict) or not _text(item.get("device_id")) for item in participants):
            return
        inventory = {item["device_id"]:item for item in participants}
        if len(inventory) != len(participants) or len({item["incident_id"] for item in active}) != len(active):
            return
        if any(item["device_id"] not in inventory or inventory[item["device_id"]].get("incident_id") != item["incident_id"] for item in active):
            return
        active_by_device = {item["device_id"]:item for item in active}
        if len(active_by_device) != len(active):
            return
        if any(item.get("incident_id") is not None and (item["device_id"] not in active_by_device or active_by_device[item["device_id"]]["incident_id"] != item["incident_id"]) for item in participants):
            return
        try:
            macs = [normalize_mac(item.get("mac")) for item in participants]
            if len([mac for mac in macs if mac]) != len({mac for mac in macs if mac}):
                return
            if any(normalize_mac(item.get("mac")) != normalize_mac(inventory[item["device_id"]].get("mac")) for item in active):
                return
        except ValueError:
            return
        if not fields["game_active"] and (active or participants):
            return
        if fields["game_active"] and not session:
            return
        ids = [item["incident_id"] for item in active] if fields["game_active"] else []
        age = max(0, (parse_utc(now)-parse_utc(event.occurred_utc)).total_seconds(), (parse_utc(now)-parse_utc(event.observed_utc)).total_seconds())
        self.db.execute("INSERT OR REPLACE INTO incident_sources VALUES (?,?,?,?,?,?,?,?,?,?)", (event.site_id,event.source_id,boot,session,event.occurred_utc,sequence,run_id,mono+FRESH_SECONDS-age,canonical_json(ids),"healthy"))
        # The new snapshot also reconciles missed end events and process restarts.
        rows = self.db.execute("SELECT * FROM incident_state WHERE site_id=? AND source_id=? AND state='open'", (event.site_id,event.source_id))
        for row in rows:
            if row["boot_id"] != boot or row["session_id"] != session or row["incident_id"] not in ids:
                self._close(row["incident_id"], "closed", "snapshot_no_longer_active", event.occurred_utc, channels)
        for item in active if fields["game_active"] else []:
            self._record_open(event, item, fields, snapshot=True)
            row = self.db.execute("SELECT * FROM incident_state WHERE incident_id=? AND site_id=? AND source_id=? AND boot_id=? AND session_id=? AND state='open'", (item["incident_id"],event.site_id,event.source_id,boot,session)).fetchone()
            if row:
                self._queue(row, channels, 1, "offline")

    def claim(self, *, run_id, mono, now_epoch):
        with self.transaction():
            ready = self.db.execute("SELECT value FROM incident_meta WHERE key='delivery_run'").fetchone()
            if not ready or ready[0] != run_id:
                return None
            # Filter eligibility in SQL BEFORE LIMIT. A previous source's stale
            # queue must never starve another source's currently eligible row.
            rows = self.db.execute("""SELECT o.*,i.site_id,i.source_id,i.boot_id,i.session_id,i.state AS incident_state
              FROM incident_outbox o JOIN incident_state i USING(incident_id)
              JOIN incident_sources s ON s.site_id=i.site_id AND s.source_id=i.source_id
              WHERE o.state IN ('pending','retryable') AND o.next_attempt<=?
                AND s.validated_run=? AND s.deadline>=?
                AND (o.version>1 OR (i.state='open' AND s.boot_id=i.boot_id AND s.session_id=i.session_id
                  AND EXISTS(SELECT 1 FROM json_each(s.active_ids) WHERE value=i.incident_id)))
              ORDER BY o.id LIMIT 1""", (now_epoch,run_id,mono)).fetchall()
            for row in rows:
                source = self.db.execute("SELECT * FROM incident_sources WHERE site_id=? AND source_id=?", (row["site_id"],row["source_id"])).fetchone()
                if not source or source["validated_run"] != run_id or source["deadline"] < mono:
                    continue
                if row["version"] == 1 and (row["incident_state"] != "open" or source["boot_id"] != row["boot_id"] or source["session_id"] != row["session_id"] or row["incident_id"] not in json.loads(source["active_ids"])):
                    continue
                self.db.execute("UPDATE incident_outbox SET state='sending',attempts=attempts+1 WHERE id=?", (row["id"],))
                result = dict(row)
                result["payload"] = json.loads(row["payload"])
                return result
        return None

    def finish(self, delivery_id, result, *, now_epoch, channels):
        state = {"delivered":"delivered", "retryable":"retryable", "permanent":"permanent", "unknown":"manual_review"}.get(result.state, "manual_review")
        with self.transaction():
            row = self.db.execute("SELECT * FROM incident_outbox WHERE id=? AND state='sending'", (delivery_id,)).fetchone()
            if not row:
                return
            if state == "retryable" and row["attempts"] >= 12:
                state = "manual_review"
            delay = max(min(3600, 2 ** min(row["attempts"], 10)), min(86400, max(0, result.retry_after)))
            self.db.execute("UPDATE incident_outbox SET state=?,next_attempt=?,error_code=?,remote_id=? WHERE id=?", (state,now_epoch+delay,result.code,result.remote_id,delivery_id))
            incident = self.db.execute("SELECT * FROM incident_state WHERE incident_id=?", (row["incident_id"],)).fetchone()
            if row["version"] == 1 and incident["state"] != "open":
                if state == "retryable":
                    self.db.execute("UPDATE incident_outbox SET state='canceled' WHERE id=?", (delivery_id,))
                elif state == "delivered":
                    self._queue(incident, channels, 2, "recovered" if incident["state"] == "recovered" else "monitoring_ended")

    def export(self, incident_id, telemetry_database=None, limit=200):
        if type(limit) is not int or not 1 <= limit <= 1000:
            raise ValueError("limit must be 1..1000")
        row = self.db.execute("SELECT * FROM incident_state WHERE incident_id=?", (incident_id,)).fetchone()
        if not row:
            raise ValueError("unknown incident")
        source = self.db.execute("SELECT health,snapshot_utc,boot_id,session_id FROM incident_sources WHERE site_id=? AND source_id=?", (row["site_id"],row["source_id"])).fetchone()
        result = {"incident": dict(row), "source_health":dict(source) if source else {"health":"unknown"}, "notifications": [dict(item) for item in self.db.execute("SELECT id,destination,version,state,attempts,error_code,remote_id FROM incident_outbox WHERE incident_id=? ORDER BY id", (incident_id,))], "related_events": [], "root_cause": "not_determined"}
        result["incident"]["details"] = json.loads(result["incident"]["details"])
        if telemetry_database:
            with closing(sqlite3.connect(Path(telemetry_database).resolve().as_uri()+"?mode=ro", uri=True)) as raw:
                if raw.execute("PRAGMA application_id").fetchone()[0] != 0x48425431:
                    raise ValueError("not a telemetry database")
                start = (parse_utc(row["opened_utc"])-timedelta(seconds=60)).isoformat(timespec="microseconds")
                end = (parse_utc(row["updated_utc"])+timedelta(seconds=60)).isoformat(timespec="microseconds")
                result["evidence_window"] = {"start_utc":start,"end_utc":end,"padding_seconds":60,
                    "unknown_device_time_rule":"same MAC only; upload arrival never substitutes for occurrence"}
                # Device flash timestamps cannot be derived from upload arrival.
                # Unknown time stays unknown and is separately labeled.
                candidates = raw.execute("SELECT e.payload FROM event_times t JOIN telemetry_events e USING(site_id,source,source_id,event_id) WHERE t.site_id=? AND ((t.source='game_server' AND t.source_id=? AND t.occurred_utc BETWEEN ? AND ? AND json_extract(e.payload,'$.fields.incident_id')=?) OR (t.mac=? AND (t.occurred_utc BETWEEN ? AND ? OR t.occurred_utc IS NULL))) ORDER BY t.occurred_utc IS NULL,t.occurred_utc LIMIT ?", (row["site_id"],row["source_id"],start,end,incident_id,row["mac"],start,end,limit+1))
                for item in candidates:
                    event = json.loads(item[0])
                    fields = event.get("fields", {})
                    exact = fields.get("incident_id") == incident_id
                    same_mac = bool(row["mac"] and event.get("mac") == row["mac"])
                    if not exact and not same_mac:
                        continue
                    unknown = event["source"] == "device" or not event.get("occurred_utc")
                    result["related_events"].append({"event":event,"relation":"same_incident" if exact else "candidate", "basis":"explicit_incident_id" if exact else "same_mac_time_unknown" if unknown else "same_mac_time_window", "time_uncertainty":"unknown" if unknown or event.get("uncertainty_ms") is None else event["uncertainty_ms"], "causality":"not_established"})
                result["truncated"] = len(result["related_events"]) > limit
                result["related_events"] = result["related_events"][:limit]
        return result

    def list_incidents(self, limit=200):
        if type(limit) is not int or not 1 <= limit <= 1000:
            raise ValueError("limit must be 1..1000")
        return [dict(row) for row in self.db.execute("SELECT incident_id,site_id,source_id,device_id,mac,session_id,state,reason,opened_utc,updated_utc FROM incident_state ORDER BY updated_utc DESC,incident_id LIMIT ?",(limit,))]

    def statistics(self):
        return {"cursor":self.cursor(),
                "incidents_by_state":dict(self.db.execute("SELECT state,count(*) FROM incident_state GROUP BY state")),
                "notifications_by_state":dict(self.db.execute("SELECT state,count(*) FROM incident_outbox GROUP BY state")),
                "sources":[dict(row) for row in self.db.execute("SELECT site_id,source_id,health AS last_recorded_health,snapshot_utc FROM incident_sources ORDER BY site_id,source_id LIMIT 1000")],
                "note":"Stored status only; use running collector health to establish current liveness. No exactly-once external delivery guarantee."}


class IncidentRuntime:
    def __init__(self, config, *, telemetry_database, site_id, source_id, check_disk=lambda: None):
        self.config, self.telemetry_database = config, telemetry_database
        self.site_id, self.source_id, self.check_disk = site_id, source_id, check_disk
        self.run_id = uuid.uuid4().hex
        self.stop_event = threading.Event()
        self.ready = threading.Event()
        self.authorized = threading.Event()
        self.threads = []
        self.counts = {"process_errors":0,"notification_errors":0}
        self.health = "awaiting_snapshot"
        notification = config.get("notifications", {})
        self.channels = [name for name in ("slack","notion") if notification.get("enabled") and notification.get(name,{}).get("enabled")]

    def start(self):
        for target,name in ((self._process_loop,"incident-state"),(self._notification_loop,"incident-delivery")):
            thread = threading.Thread(target=target, name=name, daemon=True)
            self.threads.append(thread)
            thread.start()

    def _process_loop(self):
        state = raw = None
        previous_wall, previous_mono = utc_now(), time.monotonic()
        try:
            while not self.stop_event.is_set():
                self.authorized.clear()
                try:
                    self.check_disk()
                    if state is None:
                        state = IncidentStore(self.config["database"])
                        state.start_run(self.run_id)
                    if raw is None:
                        raw = sqlite3.connect(Path(self.telemetry_database).resolve().as_uri()+"?mode=ro", uri=True, timeout=2)
                        if raw.execute("PRAGMA application_id").fetchone()[0] != 0x48425431:
                            raw.close()
                            raw = None
                            raise ValueError("not a telemetry database")
                    self.ready.set()
                    now, mono = utc_now(), time.monotonic()
                    highwater = raw.execute("SELECT coalesce(max(seq),0) FROM telemetry_ingest").fetchone()[0]
                    state.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('delivery_run','')")
                    step = (parse_utc(now)-parse_utc(previous_wall)).total_seconds()-(mono-previous_mono)
                    if abs(step)>1:
                        state.invalidate("clock_step")
                        state.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('snapshot_floor',?)", (str(highwater),))
                    previous_wall, previous_mono = now, mono
                    state.expire(self.run_id,mono)
                    rows = raw.execute("SELECT j.seq,e.payload FROM telemetry_ingest j JOIN telemetry_events e USING(site_id,source,source_id,event_id) WHERE j.seq>? ORDER BY j.seq LIMIT ?", (state.cursor(), BATCH)).fetchall()
                    for seq, payload in rows:
                        event = Event.from_dict(json.loads(payload))
                        # Cursor advances over unrelated sources without treating
                        # other stores/server identities as authorities.
                        if event.site_id != self.site_id or event.source_id != self.source_id:
                            with state.transaction():
                                state.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('cursor',?)",(str(seq),))
                        else:
                            state.process(seq,event,now=now,mono=mono,run_id=self.run_id,channels=self.channels)
                    if state.cursor() >= highwater:
                        state.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('delivery_run',?)", (self.run_id,))
                        self.authorized.set()
                    source = state.db.execute("SELECT health FROM incident_sources WHERE site_id=? AND source_id=?",(self.site_id,self.source_id)).fetchone()
                    self.health = source[0] if source else "awaiting_snapshot"
                except Exception:
                    self.authorized.clear()
                    if state:
                        try:
                            state.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('delivery_run','')")
                            state.invalidate("processing_error")
                        except sqlite3.Error:
                            pass  # The shared Event also denies the delivery worker.
                    # Publish completion only after the durable revocation was
                    # attempted. The in-memory gate closes immediately above.
                    self.health = "processing_error"
                    self.counts["process_errors"] += 1
                    if self.counts["process_errors"] % 120 == 1:
                        LOG.warning("Incident processing unavailable; raw collection remains active", exc_info=True)
                self.stop_event.wait(0.1 if 'rows' in locals() and len(rows)==BATCH else 0.5)
        finally:
            self.authorized.clear()
            if state:
                state.invalidate("collector_stopped")
                state.close()
            if raw:
                raw.close()

    def _notification_loop(self):
        state = sender = None
        try:
            while not self.stop_event.is_set():
                if not self.ready.wait(0.5):
                    continue
                if not self.authorized.wait(0.5):
                    continue
                try:
                    row = None
                    self.check_disk()
                    if state is None:
                        state = IncidentStore(self.config["database"])
                    if sender is None:
                        from notifications import NotificationSender
                        sender = NotificationSender(self.config.get("notifications", {"enabled":False}))
                    row = state.claim(run_id=self.run_id,mono=time.monotonic(),now_epoch=time.time())
                    if row:
                        if not self.authorized.is_set():
                            state.db.execute("UPDATE incident_outbox SET state='pending',attempts=attempts-1 WHERE id=? AND state='sending'", (row["id"],))
                            continue
                        result = sender.send(row["destination"],row["payload"])
                        state.finish(row["id"],result,now_epoch=time.time(),channels=self.channels)
                except Exception:
                    # An unexpected exception after claim is ambiguous; never
                    # automatically resend a possibly accepted external request.
                    self.counts["notification_errors"] += 1
                    if state and row:
                        try:
                            state.db.execute("UPDATE incident_outbox SET state='manual_review',error_code='unexpected_delivery_error' WHERE id=? AND state='sending'",(row["id"],))
                        except sqlite3.Error:
                            pass  # Restart also converts any remaining sending rows.
                    if self.counts["notification_errors"] % 120 == 1:
                        LOG.warning("Incident notification unavailable; raw collection remains active")
                self.stop_event.wait(0.5)
        finally:
            if state:
                state.close()

    def stats(self):
        return {**self.counts,"source_health":self.health,"workers":{thread.name:thread.is_alive() for thread in self.threads}}

    def stop(self, timeout=5):
        self.stop_event.set()
        end = time.monotonic()+timeout
        for thread in self.threads:
            thread.join(max(0,end-time.monotonic()))
        return all(not thread.is_alive() for thread in self.threads)


def main():
    parser = argparse.ArgumentParser(description="Export one derived incident and bounded related raw evidence")
    parser.add_argument("--database", required=True)
    parser.add_argument("--telemetry-database")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--incident-id")
    mode.add_argument("--list", action="store_true")
    mode.add_argument("--stats", action="store_true")
    parser.add_argument("--limit", type=int, default=200)
    args = parser.parse_args()
    store = IncidentStore(args.database, readonly=True)
    try:
        result = store.statistics() if args.stats else store.list_incidents(args.limit) if args.list else store.export(args.incident_id,args.telemetry_database,args.limit)
        print(json.dumps(result,ensure_ascii=False,indent=2))
    finally:
        store.close()


if __name__ == "__main__":
    main()
