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
DEVICE_FETCH = 200  # raw device records read before output-change runs are collapsed
SETTLE_SECONDS = 120  # let the post-reconnect backlog finish uploading
POLL_SECONDS = 60  # recheck for the backlog; a local read, nothing is sent while waiting
GIVE_UP_SECONDS = 6 * 3600  # a device that never returns gets server/Omada evidence only
PADDING = timedelta(seconds=60)

EVENT_KINDS = {name: kind for kind, name in EVENT_NAMES.items()}
UNAVAILABLE = -2147483648
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
# ESP-IDF esp_reset_reason_t, logged as actual_reset_reason by device.boot.
RESET_REASONS = {0: "UNKNOWN", 1: "POWERON 전원 켜짐", 2: "EXT 외부 리셋", 3: "SW 소프트웨어 재시작",
                 4: "PANIC 크래시", 5: "INT_WDT 워치독", 6: "TASK_WDT 워치독", 7: "WDT 워치독",
                 8: "DEEPSLEEP 절전 해제", 9: "BROWNOUT 전압 저하", 10: "SDIO"}
# Haptic/LED output changes repeat every few hundred ms during play and say nothing about the link.
OUTPUT_TAGS = {"motor.applied", "output.applied", "motor.suppressed"}
# What explains an outage: fetched separately and kept ahead of routine rows at the row limit.
KEY_KINDS = ("Boot", "Crash", "WifiEvent", "WifiTry", "WifiSummary", "Recovery", "Lost", "Dropped")
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
    if name == "actual_reset_reason" and value in RESET_REASONS:
        return f"{value} {RESET_REASONS[value]}"
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
            # INT32_MIN is the firmware's "no value" marker, not a reading.
            shown_values = [str(v) for v in values if v != UNAVAILABLE]
            if shown_values:
                parts.append("값 " + " ".join(shown_values))
    return " · ".join(str(part) for part in parts)


def _rows(raw, sql, params):
    return [json.loads(row[0]) for row in raw.execute(sql, params)]


def _reset_reason(event):
    values = (event.get("fields") or {}).get("values")
    if event.get("event_type") == "Boot" and (event.get("fields") or {}).get("text") == "device.boot" \
            and isinstance(values, list) and values and type(values[0]) is int:
        return f"{values[0]} {RESET_REASONS[values[0]]}" if values[0] in RESET_REASONS else str(values[0])
    return None


def _device_rows(events):
    """[(row, key)] in sequence order: output-change runs collapsed into one row, reboots marked.

    Boot ids are random per boot, so they are shown as 1, 2, ... in order of appearance.
    """
    boots, reasons, rows, run = {}, {}, [], []
    for event in events:
        reason = _reset_reason(event)
        boot = (event.get("fields") or {}).get("boot")
        if reason and boot not in reasons:
            reasons[boot] = reason

    def when(first, last=None):
        fields = first.get("fields") or {}
        uptime, received = _uptime(fields.get("uptime_ms")), _kst(first.get("observed_utc"))
        if last is not None:
            # A range only when its ends differ; several changes within one second show one time.
            last_uptime, last_received = _uptime((last.get("fields") or {}).get("uptime_ms")), _kst(last.get("observed_utc"))
            if last_uptime != uptime:
                uptime += "~" + last_uptime
            if last_received != received:
                received += "~" + last_received
        return f"부팅 {boots[fields.get('boot')]} · 켜진 뒤 {uptime} (수신 {received})"

    def flush():
        if run:
            rows.append((["장치", when(run[0], run[-1] if len(run) > 1 else None), f"모터·LED 출력 변경 {len(run)}건"], False))
            run.clear()

    previous = object()
    for event in events:
        fields = event.get("fields") or {}
        boot = fields.get("boot")
        if boot not in boots:
            boots[boot] = len(boots) + 1
        if previous in boots and boot != previous:
            flush()
            detail = f"🔁 재부팅 (부팅 {boots[previous]} → {boots[boot]})"
            rows.append((["장치", when(event), detail + (f" · 리셋 원인 {reasons[boot]}" if boot in reasons else "")], True))
        previous = boot
        if event.get("event_type") == "State" and fields.get("text") in OUTPUT_TAGS:
            run.append(event)
            continue
        flush()
        rows.append((["장치", when(event), describe_device(event)], event.get("event_type") in KEY_KINDS))
    flush()
    return rows


def _device_events(raw, incident, limit):
    """(events, more): records around the outage, and whether more than `limit` followed."""
    site, mac = incident["site_id"], incident["mac"]
    anchor = raw.execute("SELECT ring,seq FROM event_times WHERE site_id=? AND source='device' AND mac=? AND observed_utc<=? AND seq IS NOT NULL ORDER BY observed_utc DESC,seq DESC LIMIT 1",
                         (site, mac, incident["opened_utc"])).fetchone()
    select = "SELECT e.payload FROM event_times t JOIN telemetry_events e USING(site_id,source,source_id,event_id) WHERE t.site_id=? AND t.source='device' AND t.mac=? "
    key_only = "AND json_extract(e.payload,'$.event_type') IN (" + ",".join("?" * len(KEY_KINDS)) + ") "

    def merged(where, params, order):
        # Output-change spam can fill `limit` first; key records are read on their own so a
        # reboot, crash or Wi-Fi record after it is never lost to the fetch cap.
        everything = _rows(raw, select + where + order + "LIMIT ?", (*params, limit + 1))
        key = _rows(raw, select + where + key_only + order + "LIMIT ?", (*params, *KEY_KINDS, limit + 1))
        events = {}
        for event in everything[:limit] + key[:limit]:
            fields = event.get("fields") or {}
            events[(str(fields.get("ring")), fields.get("seq"), event.get("event_id"))] = event
        ordered = sorted(events.values(), key=lambda event: (str((event.get("fields") or {}).get("ring")),
                         (event.get("fields") or {}).get("seq") or 0))
        return ordered, len(everything) > limit or len(key) > limit

    if not anchor:
        # Never reported before the outage: everything it uploaded around the incident.
        end = _iso(parse_utc(incident["updated_utc"]) + timedelta(seconds=GIVE_UP_SECONDS))
        return merged("AND t.observed_utc BETWEEN ? AND ? ", (site, mac, incident["opened_utc"], end), "ORDER BY t.ring,t.seq ")
    ring, seq = anchor
    before = _rows(raw, select + "AND t.ring=? AND t.seq<=? ORDER BY t.seq DESC LIMIT ?", (site, mac, ring, seq, DEVICE_CONTEXT))
    after, more = merged("AND t.ring=? AND t.seq>? ", (site, mac, ring, seq), "ORDER BY t.seq ")
    return before[::-1] + after, more


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
        return int(min(POLL_SECONDS, GIVE_UP_SECONDS - waited)) + 1
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
    device, more = _device_events(raw, incident, DEVICE_FETCH) if incident["mac"] else ([], False)
    cut = cut or more
    device_rows = _device_rows(device)
    budget = max(0, limit - len(rows))
    if len(device_rows) > budget:
        # Over the limit, drop routine rows before boot, crash, Wi-Fi and recovery rows; if even
        # those overflow (repeated Wi-Fi retries), keep how the outage began and how it ended.
        cut = True
        key = [index for index, (_, important) in enumerate(device_rows) if important]
        if len(key) > budget:
            key = key[:budget // 2] + key[len(key) - (budget - budget // 2):]
        other = [index for index, (_, important) in enumerate(device_rows) if not important][:budget - len(key)]
        device_rows = [device_rows[index] for index in sorted(key + other)]
    rows += [row for row, _ in device_rows]
    pending = bool(incident["mac"]) and _first_upload_since_outage(raw, incident) is None
    return {"rows": [[str(cell)[:300] for cell in row] for row in rows[:limit]], "truncated": cut or len(rows) > limit,
            "device_pending": pending, "collected_utc": now, "incident_id": incident["incident_id"]}
