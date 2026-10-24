"""Import and compare site-scoped events without inventing device wall-clock time.

All imports are local and bounded. Device uptime is NOT UTC: only a documented,
explicitly bounded clock anchor can align it, and originals remain unchanged.
"""
import argparse
from dataclasses import asdict, dataclass
from datetime import timedelta
import json
import math
from pathlib import Path
import re
import sqlite3
import sys

from protocol import EVENT_NAMES, decode_record, named_values
from telemetry import (Event, MAX_EVENT_BYTES, TRUSTED_CLOCKS, TelemetryStore,
                       bounded_limit, bounded_text, canonical_json, normalize_mac,
                       normalize_utc, parse_utc, utc_now)

UPTIME_WRAP_MS = 1 << 32


def integer(value, name, minimum, maximum):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(name + " is outside allowed integer range")
    return value


@dataclass(frozen=True)
class Anchor:
    """Measured UTC interval for an explicitly identified extended device uptime.

    seq_min/max and record_uptime_wraps describe a single, externally verified
    uptime epoch. They must NOT be guessed from arrival time or adjacent rows.
    Drift expands uncertainty with distance from the measured anchor.
    """
    anchor_id: str
    site_id: str
    mac: str
    ring: str
    boot: int
    uptime_ms64: int
    utc_min: str
    utc_max: str
    origin: str
    seq_min: int
    seq_max: int
    record_uptime_wraps: int
    max_delta_ms: int = 600000
    drift_ppm: float = 1000
    clock_quality: str = "measured"

    def __post_init__(self):
        if self.clock_quality not in ("measured", "pc_unverified"):
            raise ValueError("unsupported anchor clock quality")
        for name, maximum in (("anchor_id", 256), ("site_id", 128), ("origin", 1024)):
            bounded_text(getattr(self, name), name, maximum)
        object.__setattr__(self, "mac", normalize_mac(self.mac))
        if self.mac is None or not isinstance(self.ring, str) or not re.fullmatch(r"[0-9a-f]{16}", self.ring):
            raise ValueError("anchor requires MAC and lowercase 16-hex ring")
        integer(self.boot, "boot", 0, (1 << 32) - 1)
        integer(self.uptime_ms64, "uptime_ms64", 0, (1 << 63) - 1)
        integer(self.record_uptime_wraps, "record_uptime_wraps", 0, (1 << 31) - 1)
        integer(self.seq_min, "seq_min", 1, (1 << 32) - 1)
        integer(self.seq_max, "seq_max", self.seq_min, (1 << 32) - 1)
        integer(self.max_delta_ms, "max_delta_ms", 1, 3600000)
        if (isinstance(self.drift_ppm, bool) or not isinstance(self.drift_ppm, (int, float)) or
                not math.isfinite(self.drift_ppm) or not 0 <= self.drift_ppm <= 100000):
            raise ValueError("drift_ppm must be a finite, documented bound between 0 and 100000")
        object.__setattr__(self, "utc_min", normalize_utc(self.utc_min))
        object.__setattr__(self, "utc_max", normalize_utc(self.utc_max))
        width = (parse_utc(self.utc_max) - parse_utc(self.utc_min)).total_seconds()
        if not 0 <= width <= 60:
            raise ValueError("anchor UTC interval must be ordered and at most 60 seconds")

    @classmethod
    def from_dict(cls, value):
        if not isinstance(value, dict):
            raise ValueError("anchor must be an object")
        data = dict(value)
        version = data.pop("schema_version", 1)
        if type(version) is not int or version != 1:
            raise ValueError("unsupported anchor schema")
        try:
            return cls(**data)
        except TypeError as exc:
            raise ValueError("missing or unknown anchor field") from exc

    def to_dict(self):
        return {"schema_version": 1, **asdict(self)}

    def matches(self, event):
        fields = event.fields
        return (event.source == "device" and event.site_id == self.site_id and
                event.mac == self.mac and fields.get("ring") == self.ring and
                fields.get("boot") == self.boot and
                type(fields.get("seq")) is int and self.seq_min <= fields["seq"] <= self.seq_max)

    def interval(self, event):
        if not self.matches(event):
            return None
        raw_uptime = event.fields.get("uptime_ms")
        integer(raw_uptime, "device uptime_ms", 0, UPTIME_WRAP_MS - 1)
        extended = self.record_uptime_wraps * UPTIME_WRAP_MS + raw_uptime
        delta = extended - self.uptime_ms64
        if abs(delta) > self.max_delta_ms:
            return None
        growth_ms = abs(delta) * self.drift_ppm / 1000000
        return (parse_utc(self.utc_min) + timedelta(milliseconds=delta - growth_ms),
                parse_utc(self.utc_max) + timedelta(milliseconds=delta + growth_ms))

    def selection_range(self, window=None):
        """Conservative raw-uptime search bounds; exact intervals checked later."""
        lower, upper = -self.max_delta_ms, self.max_delta_ms
        if window:
            drift = self.max_delta_ms * self.drift_ppm / 1000000
            lower = max(lower, math.floor((window[0] - parse_utc(self.utc_max)).total_seconds() * 1000 - drift))
            upper = min(upper, math.ceil((window[1] - parse_utc(self.utc_min)).total_seconds() * 1000 + drift))
        epoch = self.record_uptime_wraps * UPTIME_WRAP_MS
        raw_min = max(0, self.uptime_ms64 + lower - epoch)
        raw_max = min(UPTIME_WRAP_MS - 1, self.uptime_ms64 + upper - epoch)
        if raw_min > raw_max:
            return None
        return (self.mac, self.ring, self.boot, self.seq_min, self.seq_max, raw_min, raw_max)


def json_lines(filename, limit=10000):
    """Fail on overlong lines and report a capped import rather than silently stop."""
    bounded_limit(limit)
    with Path(filename).open("rb") as source:
        for index in range(limit + 1):
            line = source.readline(MAX_EVENT_BYTES + 2)
            if not line:
                return
            if index == limit:
                raise ValueError("import row limit exceeded; preceding rows may already be committed")
            if len(line) > MAX_EVENT_BYTES + 1:
                raise ValueError("JSONL line exceeds bounded size")
            try:
                value = json.loads(line.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                raise ValueError("invalid UTF-8 JSONL at line " + str(index + 1)) from exc
            yield value


def import_jsonl(store, filename, site_id, limit=10000):
    """Import source events; observed_utc is this collector's actual arrival time."""
    bounded_text(site_id, "site_id", 128)
    inserted = duplicates = 0
    for data in json_lines(filename, limit):
        event = Event.from_dict(data, observed_utc=utc_now())
        if event.site_id != site_id:
            raise ValueError("source site_id differs from explicitly selected site")
        if store.save(event):
            inserted += 1
        else:
            duplicates += 1
    return {"inserted": inserted, "duplicates": duplicates}


def device_event(site_id, address, ring_id, seq, device, raw, received):
    record = decode_record(raw)
    if record.seq != seq:
        raise ValueError("device DB sequence disagrees with raw record")
    return Event(site_id=site_id, source="device", source_id=address,
          event_id=ring_id + ":" + str(seq), observed_utc=received,
          occurred_utc=None, clock_quality="uptime_only", mac=address,
          device_id=device, event_type=EVENT_NAMES[record.kind],
          fields={"ring": ring_id, "seq": seq, "boot": record.boot,
                  "uptime_ms": record.uptime_ms, "uptime_bits": 32,
                  "values": list(record.values), "text": record.text,
                  "time_warning": "no UTC anchor; 49.7-day wrap epoch unknown",
                  "site_binding": "operator-selected device database"}, raw=raw.hex())


def import_device_database(store, filename, site_id, limit=10000, after_seq=0, mac=None, ring=None):
    """Read existing ACK records without changing them or using arrival as event time.

    A device database has no site field. The operator must positively bind the
    source file to a site. A DB reused across sites must first be separated.
    """
    bounded_text(site_id, "site_id", 128)
    bounded_limit(limit)
    integer(after_seq, "after_seq", 0, (1 << 32) - 1)
    mac = normalize_mac(mac)
    if ring is not None and not re.fullmatch(r"[0-9a-f]{16}", ring):
        raise ValueError("ring must be lowercase 16 hex characters")
    if after_seq and (mac is None or ring is None):
        raise ValueError("after_seq requires MAC and ring to avoid skipping other devices")
    uri = Path(filename).resolve().as_uri() + "?mode=ro"
    db = sqlite3.connect(uri, uri=True, timeout=2)
    inserted = duplicates = scanned = 0
    try:
        where, params = ["seq> ?"], [after_seq]
        for name, value in (("mac", mac), ("ring", ring)):
            if value is not None:
                where.append(name + "=?")
                params.append(value)
        query = "SELECT mac,ring,seq,device,raw,received_utc FROM records WHERE " + " AND ".join(where)
        query += " ORDER BY mac,ring,seq LIMIT ?"
        for address, ring_id, seq, device, raw, received in db.execute(query, (*params, limit + 1)):
            if scanned == limit:
                return {"inserted": inserted, "duplicates": duplicates, "truncated": True}
            event = device_event(site_id, address, ring_id, seq, device, raw, received)
            if store.save(event):
                inserted += 1
            else:
                duplicates += 1
            scanned += 1
        return {"inserted": inserted, "duplicates": duplicates, "truncated": False}
    finally:
        db.close()


def align_event(event, anchors):
    result = event.to_dict()
    if event.source != "device":
        return result
    if event.raw:
        result["decoded_values"] = named_values(decode_record(bytes.fromhex(event.raw)), event.device_id)
    # Imported device wall-clock guesses are never accepted as trusted time.
    result.update(occurred_utc=None, uncertainty_ms=None, clock_quality="uptime_only")
    lower = upper = None
    samples, count, measured = [], 0, True
    for anchor in anchors:
        interval = anchor.interval(event)
        if interval is None:
            continue
        count += 1
        measured = measured and anchor.clock_quality == "measured"
        lower = interval[0] if lower is None else max(lower, interval[0])
        upper = interval[1] if upper is None else min(upper, interval[1])
        if len(samples) < 64:
            samples.append(anchor)
    if count == 0:
        result["alignment"] = {"status": "unaligned", "reason": "no applicable measured anchor"}
        return result
    metadata = {"anchor_ids": [item.anchor_id for item in samples],
                "anchor_count": count, "anchor_details_truncated": count > len(samples)}
    if lower > upper:
        result["alignment"] = {"status": "unaligned", "reason": "conflicting measured anchors", **metadata}
        return result
    center = lower + (upper - lower) / 2
    result.update(occurred_utc=center.isoformat(timespec="microseconds"),
                  uncertainty_ms=(upper - lower).total_seconds() * 500,
                  clock_quality="anchored" if measured else "pc_unverified")
    result["alignment"] = {
        "status": "anchored", "clock_quality": result["clock_quality"], **metadata,
        "origin": [item.origin for item in samples],
        "assumptions": "explicit sequence range and uptime wrap epoch; assumed oscillator drift; see anchor clock quality",
        "drift_ppm": [item.drift_ppm for item in samples]}
    if not measured:
        result["alignment"].update(reference_clock_offset_ms=None, interval_scope="collector_pc_clock",
                                   interval_in_collector_clock=[lower.isoformat(timespec="microseconds"), upper.isoformat(timespec="microseconds")])
    return result


def event_interval(event):
    if (event.get("occurred_utc") is None or event.get("uncertainty_ms") is None or
            event.get("clock_quality") not in TRUSTED_CLOCKS):
        return None
    center = parse_utc(event["occurred_utc"])
    error = timedelta(milliseconds=event["uncertainty_ms"])
    return center - error, center + error


def export_timeline(store, output, site_id, limit=10000, window_start=None, window_end=None,
                    timestamped_after=None, device_after=None, unaligned_after=None):
    """Known-time rows precede unaligned rows; never equate arrival with occurrence."""
    bounded_limit(limit)
    if (window_start is None) != (window_end is None):
        raise ValueError("window-start and window-end are required together")
    window = None
    if window_start is not None:
        window = parse_utc(window_start), parse_utc(window_end)
        if window[0] > window[1]:
            raise ValueError("window must be ordered")
    store.prepare_anchor_query(Anchor.from_dict(value) for value in store.iter_anchor_dicts(site_id))
    normalized_window = tuple(point.isoformat(timespec="microseconds") for point in window) if window else None
    pages, events = {}, []
    for section, cursor in (("timestamped", timestamped_after), ("device_candidates", device_after),
                            ("unaligned", unaligned_after)):
        selected, more, bookmark = store.timeline_page(site_id, section, limit, normalized_window, cursor)
        pages[section] = {"examined": len(selected), "truncated": more, "next_cursor": bookmark}
        events.extend(align_event(event, (Anchor.from_dict(value) for value in store.matching_anchor_dicts(event))) for event in selected)
    events.sort(key=lambda value: (value["occurred_utc"] is None,
                                   value["occurred_utc"] or value["observed_utc"],
                                   value["source"], value["source_id"], value["event_id"]))
    total = store.event_count(site_id)
    output.write(canonical_json({"report": "timeline", "schema_version": 1, "site_id": site_id,
                                 "event_count_in_store": total, "examined": len(events),
                                 "truncated": any(page["truncated"] for page in pages.values()),
                                 "limit_per_section": limit, "sections": pages,
                                 "window_start": normalize_utc(window_start) if window else None,
                                 "window_end": normalize_utc(window_end) if window else None,
                                 "meaning": "interval overlap is a candidate, not causation; unverified clocks are not certified simultaneous"}) + "\n")
    written = 0
    for event in events:
        interval = event_interval(event)
        if event["occurred_utc"] is None:
            section, relation = "unaligned", "unknown"
        elif interval is None:
            section, relation = "timestamp_unverified", "clock_not_verified"
            local_interval = event.get("alignment", {}).get("interval_in_collector_clock")
            if local_interval:
                # Useful for relative comparison to logs from this same PC;
                # never expose this as a certified absolute UTC interval.
                if window and (parse_utc(local_interval[1]) < window[0] or parse_utc(local_interval[0]) > window[1]):
                    continue
                relation = "pc_clock_overlap_unverified" if window else "clock_not_verified"
            elif window and not window[0] <= parse_utc(event["occurred_utc"]) <= window[1]:
                continue
        else:
            section, relation = "aligned", "not_requested"
            if window:
                if interval[1] < window[0] or interval[0] > window[1]:
                    continue
                relation = "interval_overlap_candidate"
        output.write(canonical_json({"section": section, "window_relation": relation, "event": event,
                                     "interval_utc": [point.isoformat(timespec="microseconds") for point in interval] if interval else None}) + "\n")
        written += 1
    return written


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", type=Path, required=True, help="separate telemetry.sqlite3, never blackbox.sqlite3")
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("import-jsonl", "import-device", "import-anchors"):
        command = commands.add_parser(name)
        command.add_argument("input", type=Path)
        command.add_argument("--site-id", required=True)
        command.add_argument("--limit", type=int, default=10000)
        if name == "import-device":
            command.add_argument("--mac")
            command.add_argument("--ring")
            command.add_argument("--after-seq", type=int, default=0)
    command = commands.add_parser("export")
    command.add_argument("--site-id", required=True)
    command.add_argument("--limit", type=int, default=10000)
    command.add_argument("--window-start")
    command.add_argument("--window-end")
    command.add_argument("--timestamped-after", help="timestamped next_cursor from the preceding page")
    command.add_argument("--device-after", help="device_candidates next_cursor from the preceding page")
    command.add_argument("--unaligned-after", help="unaligned next_cursor from the preceding page")
    args = parser.parse_args()
    store = TelemetryStore(args.database)
    try:
        if args.command == "export":
            export_timeline(store, sys.stdout, args.site_id, args.limit, args.window_start, args.window_end,
                            args.timestamped_after, args.device_after, args.unaligned_after)
        elif args.command == "import-jsonl":
            print(canonical_json(import_jsonl(store, args.input, args.site_id, args.limit)))
        elif args.command == "import-device":
            print(canonical_json(import_device_database(store, args.input, args.site_id, args.limit,
                                                       args.after_seq, args.mac, args.ring)))
        else:
            inserted = duplicates = 0
            for value in json_lines(args.input, args.limit):
                anchor = Anchor.from_dict(value)
                if anchor.site_id != args.site_id:
                    raise ValueError("anchor site differs from explicitly selected site")
                if store.save_anchor(anchor):
                    inserted += 1
                else:
                    duplicates += 1
            print(canonical_json({"inserted": inserted, "duplicates": duplicates}))
    finally:
        store.close()


if __name__ == "__main__":
    main()
