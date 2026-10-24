"""Tiered telemetry retention: everything for a short window, then problem evidence.

`telemetry.sqlite3` otherwise grows without limit. Rows received within
``full_days`` all stay, so problems noticed later (complaints, devices that misbehaved
without a detected incident) can still be investigated. After that only these stay,
and no event row outlives ``evidence_days`` (clock anchors and import cursors are
small and kept):

- Omada and collector rows, and every game-server event except the bulk request
  and monitoring-snapshot stream (alarms, crash reports, clock anchors, mappings);
- device Boot/Crash/Recovery/Lost/Dropped records;
- incident evidence: monitoring snapshots during each incident (+/- 10 minutes),
  that device's server/gateway requests in the same window, and that device's MQTT
  records received until six hours after it (gloves upload only after the game);
- operator keep ranges added with ``keep`` (device records get the same 6 hours).

The default is a dry run. ``--apply`` deletes in short transactions so the running
collector keeps writing, checkpoints the WAL after each one so it cannot balloon,
never overtakes the incident worker's cursor, and records ``pruned_before`` so the
device importer does not re-import pruned device records. Each run classifies only
rows that crossed the full window since the last completed run; every seven days, or
with ``--rescan``, it reclassifies everything (late arrivals, released keep ranges).
SQLite reuses freed pages but only returns them to the disk in incremental
auto-vacuum mode; ``enable-shrink`` converts the file once (collector stopped).
"""
import argparse
from contextlib import closing
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

from protocol import EVENT_NAMES, decode_record
from telemetry import normalize_mac, parse_utc

APP_ID = 0x48425431
INCIDENT_APP_ID = 0x48424931
FULL_DAYS = 7
EVIDENCE_DAYS = 365
WINDOW = timedelta(minutes=10)
DEVICE_UPLOAD = timedelta(hours=6)
OPEN_INCIDENT_CAP = timedelta(hours=2)
CLOCK_STEP_GUARD = timedelta(hours=24)
CLOCK_STEP_LIMIT_MS = 3600000  # Routine time sync steps are seconds; only hour-scale jumps block.
RESCAN_EVERY = timedelta(days=7)  # Also the slack for rows observed long after they occurred.
PAGE = 2000
SHRINK_STEP_PAGES = 4096
SHRINK_PAUSE = 0.02
BUSY_MS = 30000
CHECKPOINT_BUSY_MS = 250
REQUEST_SOURCES = ("game_server", "game_gateway")
REQUEST_TYPES = ("request_ingress", "request_finish", "request_aborted", "gateway_request_finished")
BULK_SERVER_TYPES = REQUEST_TYPES + ("game_monitor_snapshot",)
DEVICE_KEEP_TYPES = ("Boot", "Crash", "Recovery", "Lost", "Dropped")
SMALL_SOURCES = ("omada", "collector")
SCHEMA = """
CREATE TABLE IF NOT EXISTS retention_keep (
  id INTEGER PRIMARY KEY AUTOINCREMENT, site_id TEXT NOT NULL, start_utc TEXT NOT NULL,
  end_utc TEXT NOT NULL, mac TEXT, device_id TEXT, note TEXT NOT NULL, created_utc TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS retention_state (
  site_id TEXT NOT NULL, key TEXT NOT NULL, value TEXT NOT NULL,
  PRIMARY KEY(site_id,key)) WITHOUT ROWID;
"""


def iso(moment):
    # Same text form as telemetry.normalize_utc, so stored timestamps compare as text.
    return moment.astimezone(timezone.utc).isoformat(timespec="microseconds")


def quoted(values):
    return ",".join("'%s'" % value for value in values)


def retention_state(db, site_id, now=None):
    """Times recorded by earlier --apply runs. A time in the future came from a wrong clock: ignored."""
    try:
        rows = db.execute("SELECT key,value FROM retention_state WHERE site_id=?", (site_id,)).fetchall()
    except sqlite3.OperationalError:
        return {}
    now = now or datetime.now(timezone.utc)
    return {key: moment for key, moment in ((key, parse_utc(value)) for key, value in rows) if moment <= now}


def retention_floors(db, site_id):
    """(pruned_before, evidence_before) recorded by the last --apply, or Nones."""
    state = retention_state(db, site_id)
    return state.get("pruned_before"), state.get("evidence_before")


def pruned_before(db, site_id):
    return retention_floors(db, site_id)[0]


def skip_pruned_device(seq, high_seq, raw, received, floors):
    """True only for a device record that retention could have imported and then pruned.

    Tail rows (above the importer's high-water mark) were never imported. Records that
    retention always keeps are imported again if absent: they were missed, not pruned.
    """
    floor, evidence_before = floors
    if floor is None or seq > high_seq:
        return False
    received = parse_utc(received)
    if received >= floor:
        return False
    kind = EVENT_NAMES.get(decode_record(raw).kind)
    return not (kind in DEVICE_KEEP_TYPES and (evidence_before is None or received >= evidence_before))


def open_telemetry(path, readonly):
    path = Path(path).resolve()
    if not path.exists():
        raise ValueError("telemetry database does not exist")
    uri = path.as_uri() + ("?mode=ro" if readonly else "")
    db = sqlite3.connect(uri, uri=True, timeout=BUSY_MS / 1000, isolation_level=None)
    try:
        db.execute("PRAGMA busy_timeout=%d" % BUSY_MS)
        if db.execute("PRAGMA application_id").fetchone()[0] != APP_ID:
            raise ValueError("not a telemetry database")
        if not readonly:
            db.execute("PRAGMA synchronous=NORMAL")
            db.executescript(SCHEMA)
        return db
    except Exception:
        db.close()
        raise


def checkpoint(db):
    """Restart the WAL between batches; a TRUNCATE checkpoint blocks writers while it waits."""
    db.execute("PRAGMA busy_timeout=%d" % CHECKPOINT_BUSY_MS)
    try:
        db.execute("PRAGMA wal_checkpoint(TRUNCATE)").fetchall()
    except sqlite3.OperationalError:
        pass
    finally:
        db.execute("PRAGMA busy_timeout=%d" % BUSY_MS)


def incident_inputs(path):
    """Incident rows and the worker's ingest cursor; deletion never passes the cursor."""
    path = Path(path).resolve()
    if not path.exists():
        raise ValueError("incidents database does not exist: " + str(path))
    with closing(sqlite3.connect(path.as_uri() + "?mode=ro", uri=True, timeout=30)) as db:
        if db.execute("PRAGMA application_id").fetchone()[0] != INCIDENT_APP_ID:
            raise ValueError("not an incidents database")
        rows = db.execute("SELECT mac,device_id,state,opened_utc,updated_utc FROM incident_state").fetchall()
        cursor = db.execute("SELECT value FROM incident_meta WHERE key='cursor'").fetchone()
    return rows, int(cursor[0]) if cursor else 0


def merged(rows):
    """Merge overlapping windows of the same kind and identity (fewer rows to probe)."""
    result = []
    for row in sorted(rows, key=lambda r: (r[0], r[1] or "", r[2] or "", r[3])):
        last = result[-1] if result else None
        if last and last[:3] == row[:3] and row[3] <= last[4]:
            result[-1] = (*last[:4], max(last[4], row[4]))
        else:
            result.append(row)
    return result


def windows(incidents, keeps, now, lower, upper):
    """Evidence windows that can still protect a row examined in [lower, upper)."""
    result, long_open = [], 0
    for mac, device_id, state, opened, updated in incidents:
        mac, opened, updated = normalize_mac(mac), parse_utc(opened), parse_utc(updated)
        end = updated
        if state == "open":
            # An incident that never closes must not keep a whole site's snapshots forever.
            end = min(now, max(opened, updated) + OPEN_INCIDENT_CAP)
            long_open += now - opened > OPEN_INCIDENT_CAP
        start, end = opened - WINDOW, end + WINDOW
        result.append(("snapshot", None, None, start, end))
        result.append(("server", mac, device_id, start, end))
        if mac:
            result.append(("device", mac, None, start, end + DEVICE_UPLOAD))
    for start, end, mac, device_id in keeps:
        start, end = parse_utc(start), parse_utc(end)
        result.append(("manual", mac, device_id, start, end))
        result.append(("manual_device", mac, device_id, start, end + DEVICE_UPLOAD))
    # Device rows can be examined up to DEVICE_UPLOAD after the window they belong to.
    # Slack covers server rows read late (occurred before observed); the weekly full scan is the backstop.
    useful = [row for row in result if row[4] >= lower - RESCAN_EVERY and row[3] <= upper + DEVICE_UPLOAD]
    return merged([(kind, mac, device, iso(start), iso(end)) for kind, mac, device, start, end in useful]), long_open


DEVICE_ID = "json_extract(e.payload,'$.device_id')"
EVENT_TYPE = "json_extract(e.payload,'$.event_type')"
CURSOR_RULE = "WHEN :cursor IS NOT NULL AND j.seq > :cursor THEN 'awaiting_incident_worker'"


def window_match(kind, time_column):
    probe = ("EXISTS(SELECT 1 FROM temp.retention_window w WHERE w.kind='{kind}' AND {column} "
             "BETWEEN w.start_utc AND w.end_utc AND {identity})")
    return " OR ".join(probe.format(kind=kind, column=time_column, identity=identity) for identity in
                       ("w.mac IS NULL AND w.device_id IS NULL", "w.mac=t.mac", "w.device_id=" + DEVICE_ID))


KEEP_RULES = """
  WHEN t.source IN ({small}) THEN 'small_source'
  WHEN t.source='game_server' AND {type} NOT IN ({bulk}) THEN 'server_event'
  WHEN t.source='device' AND {type} IN ({device}) THEN 'device_event'
  WHEN t.source<>'device' AND ({manual}) THEN 'manual_keep'
  WHEN t.source='device' AND ({manual_device}) THEN 'manual_keep'
  WHEN t.source='game_server' AND {type}='game_monitor_snapshot' AND EXISTS(
       SELECT 1 FROM temp.retention_window w WHERE w.kind='snapshot'
       AND t.occurred_utc BETWEEN w.start_utc AND w.end_utc) THEN 'incident'
  WHEN t.source IN ({request_sources}) AND {type} IN ({requests}) AND (
       EXISTS(SELECT 1 FROM temp.retention_window w WHERE w.kind='server' AND w.mac=t.mac
       AND t.occurred_utc BETWEEN w.start_utc AND w.end_utc) OR
       EXISTS(SELECT 1 FROM temp.retention_window w WHERE w.kind='server' AND w.device_id={device_id}
       AND t.occurred_utc BETWEEN w.start_utc AND w.end_utc)) THEN 'incident'
  WHEN t.source='device' AND EXISTS(SELECT 1 FROM temp.retention_window w WHERE w.kind='device'
       AND w.mac=t.mac AND t.observed_utc BETWEEN w.start_utc AND w.end_utc) THEN 'incident'
""".format(small=quoted(SMALL_SOURCES), bulk=quoted(BULK_SERVER_TYPES), device=quoted(DEVICE_KEEP_TYPES),
           request_sources=quoted(REQUEST_SOURCES), requests=quoted(REQUEST_TYPES), type=EVENT_TYPE,
           device_id=DEVICE_ID, manual=window_match("manual", "coalesce(t.occurred_utc,t.observed_utc)"),
           manual_device=window_match("manual_device", "t.observed_utc"))


def page_query(rules):
    return ("SELECT t.observed_utc,t.source,t.source_id,t.event_id," + EVENT_TYPE + " AS type,"
            "CASE " + CURSOR_RULE + rules + " END AS reason "
            "FROM event_times t JOIN telemetry_events e USING(site_id,source,source_id,event_id) "
            "LEFT JOIN telemetry_ingest j USING(site_id,source,source_id,event_id) "
            "WHERE t.site_id=:site AND t.observed_utc<:upper "
            "AND (t.observed_utc,t.source,t.source_id,t.event_id)>(:o,:s,:si,:ei) "
            "ORDER BY t.observed_utc,t.source,t.source_id,t.event_id LIMIT :page")


EXPIRE_QUERY = page_query("")
CLASSIFY_QUERY = page_query(KEEP_RULES)


def sweep(db, query, site_id, lower, upper, cursor, apply, report, page, progress):
    """Keyset pages over [lower, upper); delete unkept rows in short transactions."""
    removed = "deleted" if apply else "would_delete"
    position = (lower, "", "", "")
    while True:
        rows = db.execute(query, {"cursor": cursor, "site": site_id, "upper": upper, "o": position[0],
                                  "s": position[1], "si": position[2], "ei": position[3], "page": page}).fetchall()
        if not rows:
            return
        position = rows[-1][:4]
        doomed = [(site_id, *row[1:4]) for row in rows if row[5] is None]
        for row in rows:
            if row[5] is not None:
                report["kept"][row[5]] = report["kept"].get(row[5], 0) + 1
                if row[5] == "server_event":
                    report["kept_server_types"][row[4]] = report["kept_server_types"].get(row[4], 0) + 1
                if row[5] == "awaiting_incident_worker" and report["earliest_awaiting"] is None:
                    report["earliest_awaiting"] = row[0]  # Pages are in observed order.
        report["examined"] += len(rows)
        report[removed] += len(doomed)
        if apply and doomed:
            db.execute("BEGIN IMMEDIATE")
            try:
                for table in ("telemetry_ingest", "event_times", "telemetry_events"):
                    db.executemany("DELETE FROM " + table + " WHERE site_id=? AND source=? AND source_id=? AND event_id=?", doomed)
                db.execute("COMMIT")
            except Exception:
                db.execute("ROLLBACK")
                raise
            checkpoint(db)
        if progress:
            progress(report)


def recent_clock_step(db, site_id, now):
    try:
        return db.execute(
            "SELECT 1 FROM event_times t JOIN telemetry_events e USING(site_id,source,source_id,event_id) "
            "WHERE t.site_id=? AND t.observed_utc>=? AND t.source='collector' AND " + EVENT_TYPE + "='clock_step' "
            "AND abs(coalesce(json_extract(e.payload,'$.fields.pc_clock_step_ms'),?))>? LIMIT 1",
            (site_id, iso(now - CLOCK_STEP_GUARD), CLOCK_STEP_LIMIT_MS + 1, CLOCK_STEP_LIMIT_MS)).fetchone() is not None
    except sqlite3.OperationalError:
        return False


def prune(telemetry_path, site_id, *, incidents_path=None, protect_cursor=True, apply=False, now=None,
          full_days=FULL_DAYS, evidence_days=EVIDENCE_DAYS, rescan=False, allow_clock_step=False,
          page=PAGE, progress=None):
    if not 1 <= full_days < evidence_days:
        raise ValueError("full_days must be at least 1 and below evidence_days")
    now = now or datetime.now(timezone.utc)
    full_cutoff, evidence_cutoff = now - timedelta(days=full_days), now - timedelta(days=evidence_days)
    incidents, cursor = incident_inputs(incidents_path) if incidents_path else ([], None)
    cursor = cursor if protect_cursor else None
    started = time.monotonic()
    with closing(open_telemetry(telemetry_path, readonly=not apply)) as db:
        if apply and not allow_clock_step and recent_clock_step(db, site_id, now):
            raise ValueError("the PC clock jumped by over an hour within 24 hours; check the time, then rerun with --allow-clock-step")
        state = retention_state(db, site_id, now)
        previous, classified, scanned = (state.get(key) for key in ("pruned_before", "classified_through", "last_full_scan"))
        # Only rows that crossed the full window since the last completed run need classifying.
        # A weekly full scan catches rows that arrived late with an older observed time.
        full_scan = rescan or classified is None or scanned is None or now - scanned >= RESCAN_EVERY
        lower = evidence_cutoff if full_scan else max(evidence_cutoff, classified)
        try:
            keeps = db.execute("SELECT start_utc,end_utc,mac,device_id FROM retention_keep WHERE site_id=?", (site_id,)).fetchall()
        except sqlite3.OperationalError:
            keeps = []
        rows, long_open = windows(incidents, keeps, now, lower, full_cutoff)
        db.execute("CREATE TEMP TABLE retention_window (kind TEXT, mac TEXT, device_id TEXT, start_utc TEXT, end_utc TEXT)")
        db.execute("CREATE INDEX temp.retention_window_mac ON retention_window(kind,mac,start_utc)")
        db.execute("CREATE INDEX temp.retention_window_device ON retention_window(kind,device_id,start_utc)")
        db.executemany("INSERT INTO temp.retention_window VALUES (?,?,?,?,?)", rows)
        if apply:
            # Before any delete: a racing device import must already skip pruned records.
            marker = max(full_cutoff, previous) if previous else full_cutoff
            db.executemany("INSERT OR REPLACE INTO retention_state VALUES (?,?,?)",
                           ((site_id, "pruned_before", iso(marker)), (site_id, "evidence_before", iso(evidence_cutoff))))
        report = {"site_id": site_id, "applied": apply, "full_cutoff": iso(full_cutoff),
                  "evidence_cutoff": iso(evidence_cutoff), "full_scan": full_scan, "classified_from": iso(lower),
                  "incident_windows": len(rows), "long_open_incidents": long_open, "keep_ranges": len(keeps),
                  "examined": 0, "deleted" if apply else "would_delete": 0, "kept": {}, "kept_server_types": {},
                  "earliest_awaiting": None}
        sweep(db, EXPIRE_QUERY, site_id, "", iso(evidence_cutoff), cursor, apply, report, page, progress)
        sweep(db, CLASSIFY_QUERY, site_id, iso(lower), iso(full_cutoff), cursor, apply, report, page, progress)
        if apply:
            # Written only after both sweeps finish, so an interrupted run is redone. Rows still
            # waiting for the incident worker are classified again on the next run.
            through = min(iso(full_cutoff), report["earliest_awaiting"] or iso(full_cutoff))
            done = [(site_id, "classified_through", through)]
            if full_scan:
                done.append((site_id, "last_full_scan", iso(now)))
            db.executemany("INSERT OR REPLACE INTO retention_state VALUES (?,?,?)", done)
        report["seconds"] = round(time.monotonic() - started, 1)
        report["storage"] = shrink(db, telemetry_path) if apply else storage(db, telemetry_path)
    return report


def storage(db, path):
    page_size = db.execute("PRAGMA page_size").fetchone()[0]
    pages = db.execute("PRAGMA page_count").fetchone()[0]
    free = db.execute("PRAGMA freelist_count").fetchone()[0]
    mode = {0: "none", 1: "full", 2: "incremental"}[db.execute("PRAGMA auto_vacuum").fetchone()[0]]
    wal = Path(str(Path(path).resolve()) + "-wal")
    return {"auto_vacuum": mode, "free_mb": round(free * page_size / 1e6, 1),
            "data_mb": round((pages - free) * page_size / 1e6, 1),
            "disk_mb": round(Path(path).resolve().stat().st_size / 1e6, 1),
            "wal_mb": round(wal.stat().st_size / 1e6, 1) if wal.exists() else 0.0}


def shrink(db, path, max_seconds=600):
    """Return freed pages to the disk in short steps (incremental mode only)."""
    before = storage(db, path)
    if before["auto_vacuum"] == "incremental":
        deadline = time.monotonic() + max_seconds
        while db.execute("PRAGMA freelist_count").fetchone()[0] and time.monotonic() < deadline:
            db.execute("PRAGMA incremental_vacuum(%d)" % SHRINK_STEP_PAGES).fetchall()
            checkpoint(db)
            time.sleep(SHRINK_PAUSE)  # Let the collector's 2-second busy wait in.
    checkpoint(db)
    return {"before": before, "after": storage(db, path)}


def collector_running(service="HAS1BlackBoxCollector"):
    # A WAL database cannot tell whether another process will write next; ask Windows.
    if sys.platform != "win32":
        return False
    result = subprocess.run(["sc.exe", "query", service], capture_output=True, text=True)
    if result.returncode == 1060:  # Service not installed.
        return False
    return result.returncode != 0 or "STOPPED" not in result.stdout  # Unknown counts as running.


def enable_shrink(telemetry_path):
    """One-time VACUUM into incremental auto-vacuum. Needs the collector stopped."""
    path = Path(telemetry_path).resolve()
    if collector_running():
        raise ValueError("stop HAS1BlackBoxCollector first; VACUUM rewrites the whole file")
    with closing(open_telemetry(path, readonly=False)) as db:
        before = storage(db, path)
        if before["auto_vacuum"] == "incremental":
            return {"changed": False, "storage": before}
        # In WAL mode VACUUM writes the kept data twice: a temp copy and the new file in the WAL.
        live = before["data_mb"] * 1e6
        needs = {}
        for folder, factor in ((path.parent, 1.2), (Path(tempfile.gettempdir()), 1.1)):
            volume = os.stat(folder).st_dev
            needs[volume] = (folder, needs.get(volume, (folder, 0))[1] + factor * live)
        for folder, needed in needs.values():
            if shutil.disk_usage(folder).free < needed:
                raise ValueError("VACUUM needs about %.1f GB free on the drive of %s; run 'run --apply' first"
                                 % (needed / 1e9, folder))
        db.execute("PRAGMA auto_vacuum=INCREMENTAL")
        db.execute("VACUUM")
        checkpoint(db)
        return {"changed": True, "before": before, "after": storage(db, path)}


def add_keep(telemetry_path, site_id, start, end, mac, note, device_id=None, now=None):
    start_utc, end_utc = parse_utc(start), parse_utc(end)
    if end_utc <= start_utc:
        raise ValueError("--to must be after --from")
    if not isinstance(note, str) or not note.strip() or len(note) > 500:
        raise ValueError("--note is required (up to 500 characters)")
    if device_id is not None and (not device_id.strip() or len(device_id) > 256):
        raise ValueError("--device must be a device name")
    with closing(open_telemetry(telemetry_path, readonly=False)) as db:
        db.execute("INSERT INTO retention_keep(site_id,start_utc,end_utc,mac,device_id,note,created_utc) VALUES (?,?,?,?,?,?,?)",
                   (site_id, iso(start_utc), iso(end_utc), normalize_mac(mac), device_id, note.strip(),
                    iso(now or datetime.now(timezone.utc))))
        return db.execute("SELECT last_insert_rowid()").fetchone()[0]


def remove_keep(telemetry_path, site_id, keep_id):
    with closing(open_telemetry(telemetry_path, readonly=False)) as db:
        if not db.execute("DELETE FROM retention_keep WHERE site_id=? AND id=?", (site_id, keep_id)).rowcount:
            raise ValueError("no keep range with that id")


def list_keeps(telemetry_path, site_id):
    with closing(open_telemetry(telemetry_path, readonly=True)) as db:
        try:
            rows = db.execute("SELECT id,start_utc,end_utc,mac,device_id,note,created_utc FROM retention_keep WHERE site_id=? ORDER BY id",
                              (site_id,)).fetchall()
        except sqlite3.OperationalError:
            rows = []
    return [dict(zip(("id", "from", "to", "mac", "device", "note", "created"), row)) for row in rows]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", required=True, help="collector config.json (service: %%ProgramData%%\\HAS1BlackBox\\config.json)")
    parser.add_argument("--report", help="also append the JSON result as one line to this file (scheduled runs)")
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("run", help="report (default) or --apply retention")
    run.add_argument("--apply", action="store_true")
    run.add_argument("--full-days", type=int, default=FULL_DAYS)
    run.add_argument("--evidence-days", type=int, default=EVIDENCE_DAYS)
    run.add_argument("--rescan", action="store_true", help="reclassify every row, not only rows new since the last run")
    run.add_argument("--allow-clock-step", action="store_true", help="apply even though the PC clock jumped recently")
    keep = commands.add_parser("keep", help="keep a time range (e.g. a complaint) as evidence")
    keep.add_argument("--from", dest="start", required=True, help="ISO 8601 with offset, e.g. 2026-10-25T20:00:00+09:00")
    keep.add_argument("--to", dest="end", required=True)
    keep.add_argument("--mac")
    keep.add_argument("--device", help="device name, e.g. G2P3")
    keep.add_argument("--note", required=True)
    commands.add_parser("keeps", help="list keep ranges")
    unkeep = commands.add_parser("unkeep", help="remove a keep range by id")
    unkeep.add_argument("--id", type=int, required=True)
    commands.add_parser("enable-shrink", help="one-time conversion so pruning returns space; stop the collector first")
    args = parser.parse_args()

    from collector import load_config
    telemetry = load_config(args.config).get("telemetry")
    if not telemetry:
        raise SystemExit("config has no telemetry section")
    path, site = telemetry["database"], telemetry["site_id"]
    try:
        if args.command == "run":
            incidents = telemetry.get("incidents") or {}
            source = incidents.get("database")
            # Past incidents stay evidence even if detection is off; the cursor guard needs the worker.
            if source and not incidents.get("enabled") and not Path(source).exists():
                source = None
            result = prune(path, site, incidents_path=source, protect_cursor=bool(incidents.get("enabled")),
                           apply=args.apply, full_days=args.full_days, evidence_days=args.evidence_days,
                           rescan=args.rescan, allow_clock_step=args.allow_clock_step,
                           progress=lambda r: print("examined %d" % r["examined"], file=sys.stderr) if r["examined"] % 200000 < PAGE else None)
        elif args.command == "keep":
            result = {"id": add_keep(path, site, args.start, args.end, args.mac, args.note, args.device)}
        elif args.command == "keeps":
            result = list_keeps(path, site)
        elif args.command == "unkeep":
            remove_keep(path, site, args.id)
            result = {"removed": args.id}
        else:
            result = enable_shrink(path)
    except (ValueError, sqlite3.Error) as exc:
        raise SystemExit("retention: " + str(exc))
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if args.report:
        with open(args.report, "a", encoding="utf-8") as report:
            report.write(json.dumps({"at": iso(datetime.now(timezone.utc)), "command": args.command,
                                     "result": result}, ensure_ascii=False) + "\n")


if __name__ == "__main__":
    main()
