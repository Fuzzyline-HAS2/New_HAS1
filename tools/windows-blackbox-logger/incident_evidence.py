"""Human-readable blackbox evidence for one notified incident.

Reads the raw telemetry database only. The result is a bounded summary table of
candidate records, never a cause: device flash records carry no wall clock, so
they are shown by boot and uptime, and selected by sequence around the outage
instead of by upload arrival time.
"""
from datetime import timedelta
import json
from types import SimpleNamespace

from protocol import EVENT_NAMES, named_values
from telemetry import parse_utc

MAX_ROWS = 40
SOURCE_ROWS = 10  # per server / Omada source
DEVICE_CONTEXT = 5  # records logged before the last report preceding the outage
SETTLE_SECONDS = 120  # let the post-reconnect backlog finish uploading
GIVE_UP_SECONDS = 6 * 3600  # a device that never returns gets server/Omada evidence only
PADDING = timedelta(seconds=60)

EVENT_KINDS = {name: kind for kind, name in EVENT_NAMES.items()}
DEVICE_LABELS = {"Boot": "부팅", "Crash": "크래시", "WifiEvent": "와이파이 이벤트", "WifiTry": "와이파이 재시도",
                 "WifiSummary": "와이파이 요약", "HttpSummary": "HTTP 요약", "Recovery": "복구 동작",
                 "State": "상태", "Health": "상태 점검", "Progress": "진행", "Peer": "보조 장치",
                 "Logger": "로거", "Lost": "기록 유실", "Dropped": "기록 누락", "Acked": "전송 확인"}
SERVER_LABELS = {"device_offline": "서버가 끊김 감지", "device_recovered": "서버가 재연결 확인",
                 "alarm_cleared": "서버가 감시 종료"}
OMADA_LABELS = {"connected": "AP 접속", "disconnected": "AP 접속 해제", "roaming": "AP 로밍",
                "connection_failed": "AP 접속 실패", "device_offline": "AP 오프라인", "device_restart": "AP 재시작"}
# ESP-IDF wifi_err_reason_t values the gloves report most often.
WIFI_REASONS = {1: "UNSPECIFIED", 2: "AUTH_EXPIRE", 3: "AUTH_LEAVE", 4: "ASSOC_EXPIRE", 5: "ASSOC_TOOMANY",
                6: "NOT_AUTHED", 7: "NOT_ASSOCED", 8: "ASSOC_LEAVE", 15: "4WAY_HANDSHAKE_TIMEOUT",
                200: "BEACON_TIMEOUT", 201: "NO_AP_FOUND", 202: "AUTH_FAIL", 203: "ASSOC_FAIL",
                204: "HANDSHAKE_TIMEOUT", 205: "CONNECTION_FAIL"}
FIELD_LABELS = {"disconnect_reason": "끊김 원인", "outage_ms": "끊김 시간", "attempts": "시도",
                "actual_reset_reason": "리셋 원인", "reported_battery_mv": "배터리", "http_status": "HTTP",
                "elapsed_ms": "소요", "down_age_ms": "끊긴 지", "connect_failures": "접속 실패", "ap_bssid": "AP"}


def _iso(stamp):
    return stamp.isoformat(timespec="microseconds")


def _kst(value):
    try:
        return (parse_utc(value) + timedelta(hours=9)).strftime("%H:%M:%S")
    except (ValueError, TypeError, OverflowError):
        return "-"


def _seconds(ms):
    return f"{ms / 1000:.1f}초" if ms < 60000 else f"{ms // 60000}분 {ms % 60000 // 1000}초"


def _uptime(ms):
    if type(ms) is not int:
        return "?"
    seconds = ms // 1000
    return f"{seconds // 3600}:{seconds % 3600 // 60:02d}:{seconds % 60:02d}"


def _value(name, item):
    value, unit = item.get("value"), item.get("unit")
    if value is None or unit == "raw" or name.startswith("reserved"):
        return None
    if name == "disconnect_reason" and value in WIFI_REASONS:
        return f"{value} {WIFI_REASONS[value]}"
    if unit == "ms" and type(value) is int:
        return _seconds(value)
    if unit == "boolean":
        return "예" if value else "아니오"
    if unit == "mV":
        return f"{value}mV"
    return item.get("display", value)


def describe_device(event):
    fields = event.get("fields") or {}
    kind = event.get("event_type")
    text = fields["text"] if isinstance(fields.get("text"), str) else ""
    parts = [DEVICE_LABELS.get(kind, str(kind))] + ([text] if text else [])
    values = fields.get("values")
    if kind in EVENT_KINDS and isinstance(values, list) and len(values) == 4 and all(type(v) is int for v in values):
        decoded = named_values(SimpleNamespace(kind=EVENT_KINDS[kind], text=text, values=tuple(values)))
        shown = [(name, _value(name, item)) for name, item in decoded.get("fields", {}).items()]
        parts += [f"{FIELD_LABELS.get(name, name)} {value}" for name, value in shown if value not in (None, "")]
        if decoded.get("schema") == "unknown":
            parts.append("값 " + " ".join(str(v) for v in values))
    return " · ".join(str(part) for part in parts)


def _rows(raw, sql, params):
    return [json.loads(row[0]) for row in raw.execute(sql, params)]


def _device_events(raw, incident, limit):
    site, mac = incident["site_id"], incident["mac"]
    anchor = raw.execute("SELECT ring,seq FROM event_times WHERE site_id=? AND source='device' AND mac=? AND observed_utc<=? AND seq IS NOT NULL ORDER BY observed_utc DESC,seq DESC LIMIT 1",
                         (site, mac, incident["opened_utc"])).fetchone()
    select = "SELECT e.payload FROM event_times t JOIN telemetry_events e USING(site_id,source,source_id,event_id) WHERE t.site_id=? AND t.source='device' AND t.mac=? "
    if not anchor:
        # Never reported before the outage: everything it uploaded around the incident.
        end = _iso(parse_utc(incident["updated_utc"]) + timedelta(seconds=GIVE_UP_SECONDS))
        return _rows(raw, select + "AND t.observed_utc BETWEEN ? AND ? ORDER BY t.ring,t.seq LIMIT ?",
                     (site, mac, incident["opened_utc"], end, limit + 1))
    ring, seq = anchor
    before = _rows(raw, select + "AND t.ring=? AND t.seq<=? ORDER BY t.seq DESC LIMIT ?", (site, mac, ring, seq, DEVICE_CONTEXT))
    after = _rows(raw, select + "AND t.ring=? AND t.seq>? ORDER BY t.seq LIMIT ?", (site, mac, ring, seq, limit + 1))
    return before[::-1] + after


def _first_upload_since_outage(raw, incident):
    # The backlog can arrive before the server declares recovery, so count from the outage start.
    return raw.execute("SELECT min(observed_utc) FROM event_times WHERE site_id=? AND source='device' AND mac=? AND observed_utc>?",
                       (incident["site_id"], incident["mac"], incident["opened_utc"])).fetchone()[0]


def wait_seconds(raw, incident, now):
    """0 when evidence can be summarized now, else how long to wait for the device backlog."""
    current = parse_utc(now)
    waited = (current - parse_utc(incident["updated_utc"])).total_seconds()
    if not incident["mac"] or waited >= GIVE_UP_SECONDS:
        return 0
    first = _first_upload_since_outage(raw, incident)
    if first is None:
        return int(min(600, GIVE_UP_SECONDS - waited)) + 1
    # Settle after both the close and the first upload: the backlog spans several batches.
    elapsed = min(waited, (current - parse_utc(first)).total_seconds())
    return int(SETTLE_SECONDS - elapsed) + 1 if elapsed < SETTLE_SECONDS else 0


def summarize(raw, incident, now, limit=MAX_ROWS):
    """Rows of [출처, 시각, 내용]. Server and Omada times are KST; device times are boot/uptime."""
    start = _iso(parse_utc(incident["opened_utc"]) - PADDING)
    end = _iso(parse_utc(incident["updated_utc"]) + PADDING)
    server = _rows(raw, "SELECT e.payload FROM event_times t JOIN telemetry_events e USING(site_id,source,source_id,event_id) WHERE t.site_id=? AND t.source='game_server' AND t.source_id=? AND t.occurred_utc BETWEEN ? AND ? AND json_extract(e.payload,'$.fields.incident_id')=? AND json_extract(e.payload,'$.event_type') IN ('device_offline','device_recovered','alarm_cleared') ORDER BY t.occurred_utc LIMIT ?",
                   (incident["site_id"], incident["source_id"], start, end, incident["incident_id"], SOURCE_ROWS + 1))
    cut = len(server) > SOURCE_ROWS
    server = server[:SOURCE_ROWS]
    timed = []
    for event in server:
        fields = event.get("fields") or {}
        extra = [fields["reason"]] if isinstance(fields.get("reason"), str) else []
        if type(fields.get("gap_ms")) is int:
            extra.append("무응답 " + _seconds(fields["gap_ms"]))
        timed.append((event["occurred_utc"], ["게임 서버", _kst(event["occurred_utc"]),
                      " · ".join([SERVER_LABELS.get(event.get("event_type"), str(event.get("event_type")))] + extra)]))
    if incident["mac"]:
        # Association changes only: Omada traffic summaries carry addresses and ports.
        omada = _rows(raw, "SELECT e.payload FROM event_times t JOIN telemetry_events e USING(site_id,source,source_id,event_id) WHERE t.site_id=? AND t.source='omada' AND t.mac=? AND t.occurred_utc BETWEEN ? AND ? AND json_extract(e.payload,'$.event_type') IN ('connected','disconnected','roaming','connection_failed','device_offline','device_restart') ORDER BY t.occurred_utc LIMIT 1000",
                      (incident["site_id"], incident["mac"], start, end))
        if len(omada) > SOURCE_ROWS:
            # A flapping client: keep how the outage started and how it ended.
            cut, omada = True, omada[:SOURCE_ROWS // 2] + omada[-(SOURCE_ROWS // 2):]
        timed += [(event["occurred_utc"], ["Omada", _kst(event["occurred_utc"]), OMADA_LABELS[event["event_type"]]]) for event in omada]
    rows = [row for _, row in sorted(timed, key=lambda item: parse_utc(item[0]))]
    device = _device_events(raw, incident, limit) if incident["mac"] else []
    for event in device:
        fields = event.get("fields") or {}
        rows.append(["장치", f"부팅 {fields.get('boot', '?')} · 켜진 뒤 {_uptime(fields.get('uptime_ms'))} (수신 {_kst(event.get('observed_utc'))})",
                     describe_device(event)])
    pending = bool(incident["mac"]) and _first_upload_since_outage(raw, incident) is None
    return {"rows": [[str(cell)[:300] for cell in row] for row in rows[:limit]], "truncated": cut or len(rows) > limit,
            "device_pending": pending, "collected_utc": now, "incident_id": incident["incident_id"]}
