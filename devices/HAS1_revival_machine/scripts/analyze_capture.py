#!/usr/bin/env python3
"""capture_console.py가 만든 캡처를 읽어 태그->릴레이 구간을 분해한다 (펌웨어 v49 기준).

왜 필요한가:
  기기 로그에는 시각이 없다. capture_console.py가 받은 시각을 줄마다 붙여놓았으므로,
  여기서는 그 시각 차이로 각 HTTP 왕복과 "아무것도 찍히지 않는 침묵 구간"을 복원한다.
  태그를 유지했을 때 느려지는 시간은 로그 줄이 아니라 줄과 줄 사이에 있다.

v49 소스에서 확인한 전제 (이 분석의 근거):
  - HAS2_Wifi::HttpRequest는 request가 "Loop"가 아닐 때만 응답 payload를 찍는다
    (HAS2_Wifi.cpp:656-663). 따라서
      * 성공한 request=Loop 폴링은 콘솔에 아무것도 남기지 않는다  -> 침묵 구간으로만 보인다
      * Receive / ReceiveMine 는 JSON 본문을 통째로 찍는다        -> 셀 수 있다
      * Situation / Send 는 본문이 비면 URL이 통째로 찍힌다        -> 종류까지 구분된다
  - 승인 대기 중 폴링(PollRevivalApproval)은 ReceiveMine + DataChange 이므로
    JSON 한 줄 + "Data Change" 한 줄을 남긴다. 즉 폴링 횟수를 직접 셀 수 있다.
  - [GhostTiming] RELAY ON 줄은 SolenoidPulse(5000)의 5초 delay가 끝난 뒤에 찍힌다
    (game_state.ino:119-131). 그러므로 실제 릴레이 HIGH 시각은 이 줄의 호스트 시각보다
    약 5초 앞이다. PR #27이 바로 이걸 놓쳐 오진했으므로 여기서 명시적으로 보정한다.
  - 줄에 실린 total 값은 기기가 tagDetectedMs -> 실제 GPIO HIGH 로 직접 잰 값이라
    5초가 섞여 있지 않다. 이 값이 태그->개방 지연의 기준값이다.

사용법:
    python3 analyze_capture.py revival_ab.log
    python3 analyze_capture.py revival_ab.log --gap-ms 300 --timeline
    python3 analyze_capture.py --selftest
"""

import argparse
import re
import sys
from datetime import datetime
from statistics import median

# 실제 릴레이 통전 길이. SolenoidPulse(SOLENOID_REVIVAL_PULSE_MS) 의 블로킹 delay.
RELAY_PULSE_MS = 5000
# 이 이상 벌어진 줄 간격을 "침묵 구간"으로 보고 따로 보고한다.
DEFAULT_GAP_MS = 400

# capture_console.py 형식. 마이크로초까지 있어 침묵 구간을 잴 수 있다.
LINE_RE = re.compile(r"^(?P<ts>\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+)\s\s(?P<msg>.*)$")
# 아두이노 IDE 시리얼 모니터 형식. 초 단위라 기기가 스스로 잰 값만 신뢰할 수 있고
# 줄 사이 간격(침묵 구간) 분석은 사실상 불가능하다.
ARDUINO_LINE_RE = re.compile(r"^(?P<ts>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\s+-->\s(?P<msg>.*)$")

# 순서가 의미를 가진다. 먼저 맞는 항목이 이긴다.
EVENTS = [
    ("mark", re.compile(r"^### MARK ?(?P<text>.*)$")),
    ("tag_read", re.compile(r"^tag_user_data : (?P<user>.*)$")),
    # role= 뒤에 "(Receive took=Xms)"가 붙는 포맷(v54+)과 안 붙는 옛 포맷(v53 이전) 둘 다 받는다.
    # role을 non-greedy로 잡아야 접미사를 role 값에 먹지 않는다.
    ("role_known", re.compile(
        r"^\[RFID\] (?P<user>\S+) is_open=(?P<is_open>-?\d+) role=(?P<role>.+?)"
        r"(?: \(Receive took=(?P<receive_ms>\d+)ms\))?$")),
    ("situation_intent", re.compile(r"^\[RFID\] Tag detected - sending situation to server: (?P<user>.*)$")),
    ("situation_done", re.compile(r"^\[RFID\] Situation send (?P<user>\S+) result=(?P<result>OK|FAIL) took=(?P<took>\d+)ms$")),
    ("relay_on", re.compile(
        r"^\[GhostTiming\] RELAY ON: (?P<total>\d+)ms polls=(?P<polls>-?\d+)"
        r" role_receive=(?P<role_receive>\d+)ms situation=(?P<situation>\d+)ms"
        r" rssi_tag=(?P<rssi_tag>-?\d+) rssi_open=(?P<rssi_open>-?\d+) heap=(?P<heap>\d+)$")),
    ("ghost_timeout", re.compile(r"^\[GhostTiming\] TIMEOUT waiting for open \((?P<ms>\d+)ms\)$")),
    ("ghost_skip", re.compile(r"^\[GhostTiming\] skip - role=(?P<role>.*) \(not ghost, open not expected\)$")),
    ("approval_end", re.compile(r"^\[Approval\] wait ended: (?P<reason>.*)$")),
    ("is_open_blocked", re.compile(r"^\[RFID\] iotGlove is_open=true - blink only, no action: (?P<user>.*)$")),
    ("admin_open", re.compile(r"^\[RFID\] admin card - opening \(state-independent\)$")),
    ("reopen_ghost", re.compile(r"^\[RFID\] Tag on already-open revival machine - ghost role, opening 5s$")),
    ("reopen_blocked", re.compile(r"^\[RFID\] Tag on already-open revival machine - non-ghost role\((?P<role>.*)\), ignored$")),
    ("tagger_blocked", re.compile(r"^\[RFID\] Tag while device_state=tagger - blink only, no action$")),
    ("invalid_tag", re.compile(r"^\[RFID\] Invalid tag data \(expected G#P#\); request skipped$")),
    ("setting_open", re.compile(r"^\[RFID\] Setting tag - opening$")),
    ("outside_open", re.compile(r"^\[RFID\] Tag outside gameplay \(ready/setting\) - opening 5s without role check$")),
    ("open_confirmed", re.compile(r"^\[GameState\] device_state=open confirmed - marking is_open=1 for iotGlove: (?P<user>.*)$")),
    ("is_open_sent", re.compile(r"^\[GameState\] is_open=1 write request sent for: (?P<user>.*)$")),
    ("data_change", re.compile(r"^Data Change$")),
    ("no_server_data", re.compile(r"^\[DataChange\] 서버 데이터 없음, 스킵$")),
    # 본문이 빈 200. URL이 통째로 찍히므로 요청 종류를 알 수 있다.
    ("http_empty_ok", re.compile(r"^HTTP GET\.\.\. code: 200, empty body, request: (?P<url>.*)$")),
    ("http_bad_code", re.compile(r"^HTTP GET\.\.\. code: (?P<code>-?\d+), request: (?P<url>.*)$")),
    ("http_error_body", re.compile(r"^HTTP GET\.\.\. response body: (?P<body>.*)$")),
    ("http_failed", re.compile(r"^HTTP GET\.\.\. failed, error: (?P<error>.*?), request: (?P<url>\S+)$")),
    ("json_payload", re.compile(r"^\s*\{.*\}\s*$")),
    ("wifi_lost", re.compile(r"^WiFi disconnected\. Reconnecting\.\.\.$")),
    ("esp_restart", re.compile(r"^Restart ESP$")),
    ("telnet_connected", re.compile(r"^Telnet client connected$")),
    ("rfid_init_fail", re.compile(r"^!!!RFID 연결실패!!! - 계속 진행$")),
    ("rfid_init_ok", re.compile(r"^RFID 연결성공$")),
    ("json_parse_fail", re.compile(r"^deserializeJson\(\) failed with code ")),
]

REQUEST_RE = re.compile(r"[?&]request=(?P<request>[A-Za-z]+)")
# 개방으로 이어지는 모든 통전 경로. 유지 시행에서 재개방이 반복되는지 보려면 전부 세야 한다.
PULSE_EVENTS = {"relay_on", "admin_open", "reopen_ghost", "setting_open", "outside_open"}


class Entry:
    __slots__ = ("ts", "msg", "kind", "fields", "lineno")

    def __init__(self, ts, msg, kind, fields, lineno):
        self.ts = ts
        self.msg = msg
        self.kind = kind
        self.fields = fields
        self.lineno = lineno

    def __repr__(self):
        return f"<{self.kind} @{self.lineno}>"


def classify(msg):
    for kind, pattern in EVENTS:
        match = pattern.match(msg)
        if match:
            return kind, match.groupdict()
    return "other", {}


def parse(path):
    raw_entries, skipped, coarse = [], 0, False
    with open(path, encoding="utf-8", errors="replace") as handle:
        for lineno, raw in enumerate(handle, 1):
            line = raw.rstrip("\n")
            if not line.strip():
                continue
            match = LINE_RE.match(line)
            if not match:
                match = ARDUINO_LINE_RE.match(line)
                if match:
                    coarse = True
            if not match:
                # 기기가 개행 없이 조각을 보낸 경우 등. 버리되 셈은 해둔다.
                skipped += 1
                continue
            raw_entries.append((datetime.fromisoformat(match["ts"]), match["msg"], lineno))

    # 한 줄이 두 줄로 쪼개져 도착하는 경우가 있다(시리얼 모니터 버퍼, MTU 경계).
    # 앞뒤를 붙였을 때만 알려진 형식이 되면 원래 한 줄이었다고 보고 되붙인다.
    repaired, index = [], 0
    while index < len(raw_entries):
        ts, msg, lineno = raw_entries[index]
        if (classify(msg)[0] == "other" and index + 1 < len(raw_entries)
                and classify(raw_entries[index + 1][1])[0] == "other"
                and classify(msg + raw_entries[index + 1][1])[0] != "other"):
            repaired.append((ts, msg + raw_entries[index + 1][1], lineno))
            index += 2
            continue
        repaired.append((ts, msg, lineno))
        index += 1

    entries = []
    for ts, msg, lineno in repaired:
        kind, fields = classify(msg)
        if kind == "json_payload":
            # Receive(글러브 행)와 ReceiveMine(기기 행)은 둘 다 JSON을 찍는다.
            # 키 구성으로 가른다 - 이게 폴링 횟수를 세는 근거가 된다.
            if '"device_state"' in msg or '"game_state"' in msg:
                fields = {"row": "device"}
            elif '"role"' in msg or '"is_open"' in msg:
                fields = {"row": "glove"}
            else:
                fields = {"row": "unknown"}
        if "url" in fields:
            request = REQUEST_RE.search(fields["url"])
            fields["request"] = request["request"] if request else "?"
        entries.append(Entry(ts, msg, kind, fields, lineno))
    return entries, skipped, coarse


def ms(later, earlier):
    return (later - earlier).total_seconds() * 1000.0


def split_trials(entries):
    """### MARK 를 경계로 시행을 나눈다. 마커가 없으면 전체를 한 시행으로 본다."""
    trials, current = [], None
    for entry in entries:
        if entry.kind == "mark":
            text = (entry.fields.get("text") or "").strip()
            lowered = text.lower()
            # 도구가 스스로 넣는 마커는 시행 경계가 아니다.
            if lowered.startswith(("capture start", "capture end", "connected", "disconnected",
                                   "connect failed", "stop requested")) or not text:
                if current:
                    current["entries"].append(entry)
                continue
            current = {"label": text, "start": entry.ts, "entries": []}
            trials.append(current)
            continue
        if current is None:
            current = {"label": "(unlabelled)", "start": entry.ts, "entries": []}
            trials.append(current)
        current["entries"].append(entry)
    return trials


# CardChecking 본문이 실제로 진행됐음을 뜻하는 줄들. tag_read 뒤에 이 중 하나도 없이
# 다음 판독으로 넘어갔다면 그 판독은 v49 래치(또는 승인 대기)에서 조용히 return된 것이다.
BODY_KINDS = {
    "role_known", "situation_intent", "situation_done", "relay_on", "ghost_timeout", "ghost_skip",
    "approval_end", "is_open_blocked", "admin_open", "reopen_ghost", "reopen_blocked",
    "tagger_blocked", "invalid_tag", "setting_open", "outside_open", "open_confirmed", "is_open_sent",
}
# 개방 뒤 이 시간 안에 같은 태그가 다시 읽히면 "붙여두고 있었다"고 본다.
# RELAY ON 줄은 통전 5초가 끝난 뒤 찍히므로, 뗐다면 그 뒤 재판독이 있을 수 없다.
HELD_WINDOW_MS = 2000


def _raw_cycles(entries):
    """tag_read 부터 그 태그가 낳은 결말까지를 한 사이클로 묶는다 (접기 전)."""
    cycles = []
    open_cycle = None
    for entry in entries:
        if entry.kind == "tag_read":
            if open_cycle is not None:
                open_cycle["end"] = entry
                open_cycle["outcome"] = open_cycle.get("outcome") or "superseded by next tag"
                cycles.append(open_cycle)
            open_cycle = {"start": entry, "entries": [entry], "end": None, "outcome": None,
                          "user": entry.fields.get("user", "?")}
            continue
        if open_cycle is None:
            continue
        open_cycle["entries"].append(entry)
        if entry.kind == "relay_on":
            open_cycle["end"] = entry
            open_cycle["outcome"] = "relay opened (ghost first open)"
            cycles.append(open_cycle)
            open_cycle = None
        elif entry.kind in ("ghost_timeout",):
            open_cycle["end"] = entry
            open_cycle["outcome"] = "approval timeout (15s) - relay never opened"
            cycles.append(open_cycle)
            open_cycle = None
        elif entry.kind in ("is_open_blocked", "tagger_blocked", "invalid_tag", "reopen_blocked"):
            open_cycle["end"] = entry
            open_cycle["outcome"] = f"rejected locally ({entry.kind})"
            cycles.append(open_cycle)
            open_cycle = None
        elif entry.kind in ("admin_open", "reopen_ghost", "setting_open", "outside_open"):
            open_cycle["end"] = entry
            open_cycle["outcome"] = f"pulsed without server approval ({entry.kind})"
            cycles.append(open_cycle)
            open_cycle = None
        elif entry.kind == "approval_end":
            # 유령 태그의 timeout은 바로 앞 [GhostTiming] TIMEOUT에서 이미 닫혔고
            # (approval.ino:49-53의 출력 순서), 그 경우 이 줄은 여기 오지 않는다.
            # 유령이 아닌 태그의 timeout은 TIMEOUT 줄이 없으므로 여기서 닫아야 한다.
            open_cycle["end"] = entry
            open_cycle["outcome"] = f"approval wait ended: {entry.fields.get('reason')}"
            cycles.append(open_cycle)
            open_cycle = None
    if open_cycle is not None:
        open_cycle["outcome"] = "unterminated (capture ended or tag removed)"
        open_cycle["end"] = open_cycle["entries"][-1]
        cycles.append(open_cycle)
    return cycles


def _is_suppressed(cycle):
    return not any(e.kind in BODY_KINDS for e in cycle["entries"][1:])


def find_cycles(entries):
    """사이클을 묶되, v49 래치에 막힌 재판독(tag_user_data만 찍히고 끝)은 별도 사이클로
    흩뿌리지 않고 직전 같은 태그의 개방/통전 사이클에 "붙여둔 판독"으로 접어 넣는다.
    이 재판독의 존재 여부와 시각이 유지/제거를 마커 없이도 가른다."""
    folded = []
    last_real = None
    for cycle in _raw_cycles(entries):
        cycle.setdefault("held_reads", [])
        cycle.setdefault("suppressed_count", 0)
        if not _is_suppressed(cycle):
            cycle["auto_hold"] = None
            folded.append(cycle)
            last_real = cycle
            continue
        # 래치/대기에 막힌 판독. 직전 같은 사용자의 실제 사이클에 붙인다.
        if last_real is not None and last_real["user"] == cycle["user"]:
            last_real["held_reads"].append(cycle["start"])
            # 그 사이에 섞인 배경 이벤트(Data Change 등)도 잃지 않고 붙여둔다.
            last_real.setdefault("after_entries", []).extend(cycle["entries"])
            continue
        # 앞선 사이클이 없거나 사용자가 다르면 같은 사용자의 연속 억제 판독끼리 묶는다.
        if folded and folded[-1].get("outcome") == "latched reads only" and folded[-1]["user"] == cycle["user"]:
            folded[-1]["suppressed_count"] += 1
            folded[-1]["held_reads"].append(cycle["start"])
            folded[-1]["end"] = cycle["end"]
            continue
        cycle["outcome"] = "latched reads only"
        cycle["suppressed_count"] = 1
        cycle["held_reads"] = [cycle["start"]]
        cycle["auto_hold"] = None
        folded.append(cycle)

    # 개방/통전 사이클마다 유지/제거를 판별한다.
    for index, cycle in enumerate(folded):
        if cycle["outcome"] == "latched reads only" or cycle["end"] is None:
            continue
        if cycle["end"].kind not in PULSE_EVENTS:
            continue
        anchor = cycle["end"].ts
        held = any(0 <= ms(r.ts, anchor) <= HELD_WINDOW_MS for r in cycle["held_reads"])
        if not held:
            # v48처럼 래치가 없으면 붙여둔 태그가 재개방 사이클로 나타난다.
            for later in folded[index + 1:]:
                if later["user"] != cycle["user"]:
                    break
                if later["end"] is not None and later["end"].kind == "reopen_ghost" \
                        and 0 <= ms(later["start"].ts, anchor) <= HELD_WINDOW_MS:
                    held = True
                break
        cycle["auto_hold"] = held
    return folded


def describe_gap(prev, nxt):
    """두 줄 사이에 무엇이 일어났는지 v49의 제어흐름으로 설명한다."""
    # 이 검사가 가장 먼저여야 한다. RELAY ON 줄 직전 간격에는 5초 통전이 통째로
    # 들어 있어서, 다른 설명으로 분류되면 침묵 구간 합계에서 5초가 빠지지 않는다.
    if nxt.kind == "relay_on":
        return f"릴레이 HIGH + {RELAY_PULSE_MS}ms 통전 (이 줄은 통전이 끝난 뒤 찍힌다)"
    if prev.kind == "tag_read":
        if nxt.kind == "role_known" or nxt.kind == "json_payload":
            return "has2wifi.Receive() 왕복 (is_open/role 조회)"
        # 판독은 됐지만 CardChecking 본문이 진행되지 않았다 = v49 래치 또는 승인 대기에서
        # return. 그 뒤의 침묵은 기기가 한 일이 아니라 다음 서버 변경/태그까지의 대기다.
        return "래치된 판독 뒤 대기 (기기 작업 없음 - 서버 재무장이나 다음 태그를 기다림)"
    if prev.kind in ("role_known", "situation_intent"):
        return "has2wifi.Situation() 왕복"
    if prev.kind == "situation_done":
        return "첫 승인 조회 (PollRevivalApproval -> ReceiveMine)"
    if prev.kind == "data_change":
        return "다음 승인 폴링까지 대기 + ReceiveMine 왕복 (또는 침묵인 request=Loop)"
    if prev.kind == "json_payload" and prev.fields.get("row") == "device":
        return "DataChange() 처리"
    if nxt.kind == "relay_on":
        return f"릴레이 HIGH + {RELAY_PULSE_MS}ms 통전 (이 줄은 통전이 끝난 뒤 찍힌다)"
    if prev.kind == "is_open_blocked":
        return "NeoBlinkPurple(3) 블로킹 delay 약 900ms + 색 복원"
    if prev.kind in ("admin_open", "reopen_ghost", "outside_open"):
        return f"릴레이 HIGH + {RELAY_PULSE_MS}ms 통전"
    if prev.kind == "setting_open":
        return "릴레이 HIGH + SOLENOID_PULSE_MS 통전"
    return "설명 없음 - 콘솔에 남지 않는 작업 (PN532 판독/ApplyGain, 성공한 request=Loop 폴링, delay 등)"


def display_width(text):
    """한글 등 전각 문자를 2칸으로 세어 표 정렬을 맞춘다."""
    import unicodedata
    return sum(2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1 for ch in text)


def pad(text, width):
    return text + " " * max(0, width - display_width(text))


def analyse_cycle(cycle, gap_ms):
    entries = cycle["entries"]
    start = cycle["start"]
    held_reads = cycle.get("held_reads", [])
    anchor = cycle["end"].ts if cycle.get("end") is not None else None
    info = {
        "user": cycle["user"],
        "outcome": cycle["outcome"],
        "auto_hold": cycle.get("auto_hold"),
        "held_read_count": len(held_reads),
        "suppressed_count": cycle.get("suppressed_count", 0),
        # RELAY ON 줄(=통전 종료) 기준으로 같은 태그가 마지막으로 읽힌 시각. 얼마나 오래 붙여뒀는지.
        "held_until_ms": (max(ms(r.ts, anchor) for r in held_reads) if held_reads and anchor else None),
        "start_ts": start.ts,
        "lineno": start.lineno,
        "device_total_ms": None,
        "device_polls": None,
        "role_receive_ms": None,
        "situation_ms": None,
        "rssi_tag": None,
        "host_relay_high_ms": None,
        "clock_skew_ms": None,
        "receive_calls": 0,
        "receivemine_calls": 0,
        "situation_calls": 0,
        "send_calls": 0,
        "data_changes": 0,
        "repeat_tag_reads": 0,
        "http_failures": 0,
        "gaps": [],
        "warnings": [],
    }

    for entry in entries:
        kind, fields = entry.kind, entry.fields
        if kind == "json_payload":
            if fields.get("row") == "glove":
                info["receive_calls"] += 1
            elif fields.get("row") == "device":
                info["receivemine_calls"] += 1
        elif kind == "role_known" and fields.get("receive_ms") is not None:
            # v54+ 로그는 [RFID] 줄 자체에 Receive() 왕복시간을 남긴다. relay_on(유령 개방)이
            # 없는 사이클(대부분의 player/revival 거절)도 이 값으로 role_receive_ms를 채울 수 있다.
            # relay_on이 나중에 나오면 같은 값을 다시 써서 덮어쓰므로 충돌 없다.
            info["role_receive_ms"] = int(fields["receive_ms"])
        elif kind in ("http_empty_ok", "http_bad_code", "http_failed"):
            # 한 번의 HTTP 호출은 payload 줄 "또는" URL 줄 중 하나만 남긴다
            # (HttpRequest: 본문이 있으면 본문, 비면 URL). 그래서 겹쳐 세지 않는다.
            # Situation은 본문이 비지 않으면 URL이 안 남으므로 여기서 세지 않고
            # 아래 situation_done 줄로 센다 - 그쪽이 호출당 정확히 1회다.
            request = fields.get("request", "?")
            if request == "Send":
                info["send_calls"] += 1
            elif request == "ReceiveMine":
                info["receivemine_calls"] += 1
            elif request == "Receive":
                info["receive_calls"] += 1
            if kind in ("http_bad_code", "http_failed"):
                info["http_failures"] += 1
                info["warnings"].append(
                    f"HTTP 실패 (line {entry.lineno}): {entry.msg[:140]}")
        elif kind == "data_change":
            info["data_changes"] += 1
        elif kind == "tag_read" and entry is not start:
            info["repeat_tag_reads"] += 1
        elif kind == "relay_on":
            info["device_total_ms"] = int(fields["total"])
            info["device_polls"] = int(fields["polls"])
            info["role_receive_ms"] = int(fields["role_receive"])
            info["situation_ms"] = int(fields["situation"])
            info["rssi_tag"] = int(fields["rssi_tag"])
            host_line = ms(entry.ts, start.ts)
            info["host_relay_high_ms"] = host_line - RELAY_PULSE_MS
            info["clock_skew_ms"] = info["host_relay_high_ms"] - info["device_total_ms"]
        elif kind == "situation_done":
            # Situation 호출당 정확히 한 번 찍히는 줄 (sensor.ino:302).
            info["situation_calls"] += 1
            info["situation_ms"] = info["situation_ms"] or int(fields["took"])
            if fields["result"] == "FAIL":
                info["warnings"].append("Situation 전송 실패 - 서버에 태그 이벤트가 닿지 않았다")
        elif kind == "esp_restart":
            info["warnings"].append("이 사이클 도중 ESP가 재시작했다 - 측정값 무효")
        elif kind == "wifi_lost":
            info["warnings"].append("이 사이클 도중 Wi-Fi가 끊겼다 - 측정값 주의")

    previous = None
    for entry in entries:
        if previous is not None:
            gap = ms(entry.ts, previous.ts)
            if gap >= gap_ms:
                info["gaps"].append({
                    "ms": gap,
                    "after": previous.msg[:100],
                    "before": entry.msg[:100],
                    "why": describe_gap(previous, entry),
                    "lineno": entry.lineno,
                })
        previous = entry

    # 실제 릴레이 통전 5초는 지연이 아니므로 침묵 구간 합계에서 뺀다.
    info["silent_total_ms"] = sum(
        g["ms"] - (RELAY_PULSE_MS if "통전" in g["why"] else 0) for g in info["gaps"])

    if info["clock_skew_ms"] is not None and abs(info["clock_skew_ms"]) > 400:
        info["warnings"].append(
            f"호스트 시각 기준 개방시점과 기기 total 값이 {info['clock_skew_ms']:.0f}ms 어긋난다 "
            "- 캡처 지연이나 시리얼 적체를 의심하라")
    if info["situation_calls"] > 1:
        info["warnings"].append(
            f"같은 사이클에서 Situation이 {info['situation_calls']}회 나갔다 "
            "- v49 래치(gameplay_tag_latched)가 새고 있다")
    if info["repeat_tag_reads"] > 0:
        info["warnings"].append(
            f"승인 결말 전에 tag_user_data가 {info['repeat_tag_reads']}회 더 찍혔다 "
            "- 붙여둔 태그의 재판독이 막히지 않았다")
    if info["receive_calls"] > 1:
        info["warnings"].append(
            f"role 조회(Receive)가 {info['receive_calls']}회 - 반복마다 왕복 비용을 다시 냈다")
    return info


def fmt(value, suffix="", width=8):
    if value is None:
        return "-".rjust(width)
    if isinstance(value, float):
        return f"{value:,.0f}{suffix}".rjust(width)
    return f"{value:,}{suffix}".rjust(width)


def classify_label(label):
    lowered = label.lower()
    if any(k in lowered for k in ("hold", "held", "유지")):
        return "HELD"
    if any(k in lowered for k in ("release", "remove", "제거", "뗌", "떼")):
        return "RELEASED"
    return "OTHER"


def report(trials, gap_ms, show_timeline):
    groups = {}
    print("=" * 100)
    print("시행별 분석")
    print("=" * 100)

    for trial in trials:
        cycles = find_cycles(trial["entries"])
        kind = classify_label(trial["label"])
        print(f"\n■ {trial['label']}   [{kind}]   {trial['start'].isoformat(timespec='milliseconds')}")
        if not cycles:
            print("   태그 판독이 없다 (tag_user_data 줄 없음).")
            continue
        for index, cycle in enumerate(cycles, 1):
            info = analyse_cycle(cycle, gap_ms)
            # 마커가 유지/제거를 말해주지 않으면 로그 자체(개방 뒤 같은 태그 재판독)로 가른다.
            group_key = kind
            if kind == "OTHER" and info["auto_hold"] is not None:
                group_key = "HELD(auto)" if info["auto_hold"] else "RELEASED(auto)"
            # A/B 비교는 서버 승인을 거친 첫 개방만으로 한다. 관리자/재개방 펄스는
            # 왕복이 없거나 다른 경로라 섞이면 중앙값을 흐린다.
            if cycle.get("end") is not None and cycle["end"].kind == "relay_on":
                groups.setdefault(group_key, []).append(info)
            if info["outcome"] == "latched reads only":
                print(f"   [{index}] glove={info['user']}  래치/대기에 막힌 판독 {info['suppressed_count']}회"
                      f" - 처리 없음  (line {info['lineno']}~)")
                continue
            print(f"   [{index}] glove={info['user']}  결과: {info['outcome']}  (line {info['lineno']})")
            if info["auto_hold"] is not None:
                if info["auto_hold"] and info["held_read_count"]:
                    print(f"       태그 유지 판별              : 유지(HELD) - 통전 종료 뒤에도 같은 태그가"
                          f" {info['held_read_count']}회 읽힘 (마지막 +{info['held_until_ms']/1000:.1f}s),"
                          f" 재개방 없음 = v49 래치 정상")
                elif info["auto_hold"]:
                    # 래치 없는 펌웨어(v48)에서는 붙여둔 태그가 곧바로 재개방 펄스로 나타난다.
                    print("       태그 유지 판별              : 유지(HELD) - 통전 종료 직후 같은 태그가 재개방"
                          " 펄스를 냈다 (래치 없음 = v48 동작)")
                else:
                    print(f"       태그 유지 판별              : 제거(RELEASED) - 통전 종료 후"
                          f" {HELD_WINDOW_MS/1000:.0f}s 안에 같은 태그 재판독 없음")
            print(f"       기기 실측 태그->릴레이 HIGH : {fmt(info['device_total_ms'], 'ms')}"
                  f"   (role_receive {fmt(info['role_receive_ms'], 'ms', 6)},"
                  f" situation {fmt(info['situation_ms'], 'ms', 6)}, polls {fmt(info['device_polls'], '', 4)})")
            if info["host_relay_high_ms"] is not None:
                print(f"       호스트 시각 기준 교차검증    : {fmt(info['host_relay_high_ms'], 'ms')}"
                      f"   (RELAY ON 줄 시각 - {RELAY_PULSE_MS}ms, 차이 {info['clock_skew_ms']:+.0f}ms)")
            print(f"       HTTP 왕복 횟수               : Receive {info['receive_calls']},"
                  f" Situation {info['situation_calls']},"
                  f" ReceiveMine {info['receivemine_calls']},"
                  f" Send {info['send_calls']},"
                  f" DataChange {info['data_changes']}")
            if info["gaps"]:
                print(f"       {gap_ms}ms 이상 침묵 구간 (합계 {info['silent_total_ms']:,.0f}ms):")
                for gap in info["gaps"]:
                    print(f"         · {gap['ms']:8,.0f}ms  {gap['why']}")
                    print(f"                      ← {gap['after']}")
                    print(f"                      → {gap['before']}")
            for warning in info["warnings"]:
                print(f"       ⚠ {warning}")
            if show_timeline:
                print("       타임라인:")
                base = cycle["start"].ts
                for entry in cycle["entries"]:
                    print(f"         {ms(entry.ts, base):9,.0f}ms  {entry.kind:18s} {entry.msg[:88]}")

    print()
    print("=" * 100)
    print("A/B 비교")
    print("=" * 100)
    if not groups:
        print("비교할 사이클이 없다.")
        return

    def summarise(values):
        values = [v for v in values if v is not None]
        if not values:
            return "-"
        if len(values) == 1:
            return f"{values[0]:,.0f}"
        return f"중앙값 {median(values):,.0f}  (min {min(values):,.0f} / max {max(values):,.0f}, n={len(values)})"

    rows = [
        ("기기 실측 태그->릴레이 (ms)", lambda c: c["device_total_ms"]),
        ("그중 role 조회 (ms)", lambda c: c["role_receive_ms"]),
        ("그중 Situation (ms)", lambda c: c["situation_ms"]),
        ("승인 폴링 횟수 (polls)", lambda c: c["device_polls"]),
        ("침묵 구간 합계 (ms)", lambda c: c["silent_total_ms"]),
        ("Receive 호출 수", lambda c: c["receive_calls"]),
        ("Situation 호출 수", lambda c: c["situation_calls"]),
        ("사이클 내 재판독 수", lambda c: c["repeat_tag_reads"]),
        ("개방 후 같은 태그 재판독 수", lambda c: c["held_read_count"]),
    ]
    order = [k for k in ("HELD", "HELD(auto)", "RELEASED", "RELEASED(auto)", "OTHER") if k in groups]
    width = max(display_width(r[0]) for r in rows) + 2
    print(pad("항목", width) + "".join(pad(k, 46) for k in order))
    for name, getter in rows:
        line = pad(name, width)
        for key in order:
            line += pad(summarise([getter(c) for c in groups[key]]), 46)
        print(line)

    print()
    print("판정 가이드")
    print("-" * 100)
    held = groups.get("HELD", []) + groups.get("HELD(auto)", [])
    released = groups.get("RELEASED", []) + groups.get("RELEASED(auto)", [])
    held_totals = [c["device_total_ms"] for c in held if c["device_total_ms"] is not None]
    rel_totals = [c["device_total_ms"] for c in released if c["device_total_ms"] is not None]
    if held_totals and rel_totals:
        delta = median(held_totals) - median(rel_totals)
        print(f"  유지 - 제거 차이(중앙값): {delta:+,.0f}ms")
        if abs(delta) < 200:
            print("  → 유의미한 차이가 없다. 증상이 재현되지 않았다면 조건(혼잡도/글러브/상태)을 다시 맞춰야 한다.")
        else:
            held_leak = sum(c["situation_calls"] > 1 or c["repeat_tag_reads"] > 0 for c in held)
            if held_leak:
                print(f"  → 유지 시행 {held_leak}건에서 래치가 샜다. v49의 gameplay_tag_latched가 "
                      "이 경로를 막지 못하고 있다 (H1/H2 성립).")
            else:
                print("  → 래치는 정상 동작했는데도 느리다. 반복 판독이 원인이 아니므로 "
                      "H1/H2는 기각되고, 남는 것은 폴링 구조(H3)나 PN532(H4)다.")
            held_role = [c["role_receive_ms"] for c in held if c["role_receive_ms"] is not None]
            rel_role = [c["role_receive_ms"] for c in released if c["role_receive_ms"] is not None]
            if held_role and rel_role and abs(median(held_role) - median(rel_role)) > 150:
                print("  → role 조회 왕복 자체가 유지 시행에서 더 길다. 기기가 아니라 "
                      "서버/AP 지연을 보고 있는 것이다.")
            held_silent = [c["silent_total_ms"] for c in held]
            rel_silent = [c["silent_total_ms"] for c in released]
            if held_silent and rel_silent and median(held_silent) - median(rel_silent) > 200:
                print("  → 차이가 로그에 남지 않는 침묵 구간에 있다. 콘솔에 안 찍히는 작업은 "
                      "PN532 판독/ApplyGain, 성공한 request=Loop 폴링, delay 뿐이다 (H3/H4).")
    else:
        print("  유지/제거 양쪽 모두에서 릴레이가 열린 사이클이 있어야 비교할 수 있다.")
        print("  마커 문구에 hold / release 를 포함시켰는지, 글러브 is_open이 0이었는지 확인하라.")


SELFTEST_LOG = """\
2026-09-19T07:00:00.000000  ### MARK capture start host=172.30.1.9:23
2026-09-19T07:00:00.100000  ### MARK connected
2026-09-19T07:00:01.000000  ### MARK B1 release
2026-09-19T07:00:02.000000  tag_user_data : G1P1
2026-09-19T07:00:02.313000  {"device_name":"G1P1","role":"ghost","is_open":0}
2026-09-19T07:00:02.315000  [RFID] G1P1 is_open=0 role=ghost
2026-09-19T07:00:02.316000  [RFID] Tag detected - sending situation to server: G1P1
2026-09-19T07:00:02.537000  HTTP GET... code: 200, empty body, request: http://172.30.1.43?request=Situation&table=revival_machine&key=revival_machine_original&value=G1P1
2026-09-19T07:00:02.538000  [RFID] Situation send G1P1 result=OK took=221ms
2026-09-19T07:00:02.820000  {"device_name":"revival_machine_original","game_state":"activate","device_state":"open"}
2026-09-19T07:00:02.821000  Data Change
2026-09-19T07:00:07.835000  [GhostTiming] RELAY ON: 825ms polls=1 role_receive=313ms situation=221ms rssi_tag=-58 rssi_open=-59 heap=142312
2026-09-19T07:00:07.840000  [GameState] device_state=open confirmed - marking is_open=1 for iotGlove: G1P1
2026-09-19T07:00:07.900000  [GameState] is_open=1 write request sent for: G1P1
2026-09-19T07:00:20.000000  ### MARK A1 hold
2026-09-19T07:00:21.000000  tag_user_data : G2P2
2026-09-19T07:00:21.320000  {"device_name":"G2P2","role":"ghost","is_open":0}
2026-09-19T07:00:21.322000  [RFID] G2P2 is_open=0 role=ghost
2026-09-19T07:00:21.323000  [RFID] Tag detected - sending situation to server: G2P2
2026-09-19T07:00:21.560000  HTTP GET... code: 200, empty body, request: http://172.30.1.43?request=Situation&table=revival_machine&key=revival_machine_original&value=G2P2
2026-09-19T07:00:21.561000  [RFID] Situation send G2P2 result=OK took=238ms
2026-09-19T07:00:21.850000  {"device_name":"revival_machine_original","game_state":"activate","device_state":"activate"}
2026-09-19T07:00:21.851000  Data Change
2026-09-19T07:00:23.900000  {"device_name":"revival_machine_original","game_state":"activate","device_state":"activate"}
2026-09-19T07:00:23.901000  Data Change
2026-09-19T07:00:24.800000  {"device_name":"revival_machine_original","game_state":"activate","device_state":"open"}
2026-09-19T07:00:24.801000  Data Change
2026-09-19T07:00:29.815000  [GhostTiming] RELAY ON: 3805ms polls=3 role_receive=320ms situation=238ms rssi_tag=-71 rssi_open=-70 heap=141880
2026-09-19T07:00:29.820000  [GameState] device_state=open confirmed - marking is_open=1 for iotGlove: G2P2
2026-09-19T07:00:35.000000  ### MARK capture end
"""


def selftest():
    import tempfile
    import os
    handle = tempfile.NamedTemporaryFile("w", suffix=".log", delete=False, encoding="utf-8")
    handle.write(SELFTEST_LOG)
    handle.close()
    try:
        entries, skipped, _ = parse(handle.name)
        assert skipped == 0, skipped
        trials = split_trials(entries)
        labels = [t["label"] for t in trials]
        assert labels == ["B1 release", "A1 hold"], labels

        released = analyse_cycle(find_cycles(trials[0]["entries"])[0], DEFAULT_GAP_MS)
        held = analyse_cycle(find_cycles(trials[1]["entries"])[0], DEFAULT_GAP_MS)

        assert released["device_total_ms"] == 825, released["device_total_ms"]
        assert released["device_polls"] == 1
        assert released["receive_calls"] == 1, released["receive_calls"]
        assert released["situation_calls"] == 1, released["situation_calls"]
        assert released["receivemine_calls"] == 1, released["receivemine_calls"]
        assert released["repeat_tag_reads"] == 0
        # RELAY ON 줄은 5초 통전 뒤에 찍히므로 보정 후 기기 total과 맞아야 한다.
        assert abs(released["clock_skew_ms"]) < 20, released["clock_skew_ms"]
        assert not released["warnings"], released["warnings"]

        assert held["device_total_ms"] == 3805
        assert held["device_polls"] == 3
        assert held["receivemine_calls"] == 3, held["receivemine_calls"]
        assert held["situation_calls"] == 1, held["situation_calls"]
        assert abs(held["clock_skew_ms"]) < 20, held["clock_skew_ms"]
        # 유지 쪽은 폴링 사이에 2초짜리 침묵이 있어야 한다.
        assert any(g["ms"] > 1900 for g in held["gaps"]), held["gaps"]

        assert classify_label("A1 hold") == "HELD"
        assert classify_label("B1 release") == "RELEASED"
        assert classify_label("D1 admin MMMM") == "OTHER"

        # 실패 경로 분류도 확인한다.
        _, fields = classify("[Approval] wait ended: timeout"), None
        assert classify("[Approval] wait ended: timeout")[0] == "approval_end"
        assert classify("[GhostTiming] TIMEOUT waiting for open (15000ms)")[0] == "ghost_timeout"
        assert classify("[RFID] Tag on already-open revival machine - ghost role, opening 5s")[0] == "reopen_ghost"
        assert classify("[RFID] iotGlove is_open=true - blink only, no action: G1P1")[0] == "is_open_blocked"
        assert classify("HTTP GET... failed, error: connection refused, request: http://x?request=Loop")[0] == "http_failed"
        code_kind, code_fields = classify("HTTP GET... code: 500, request: http://x?request=ReceiveMine&mac=AA")
        assert code_kind == "http_bad_code" and code_fields["code"] == "500"
        empty_kind, empty_fields = classify(
            "HTTP GET... code: 200, empty body, request: http://x?request=Send&key=G1P1")
        assert empty_kind == "http_empty_ok", empty_kind
        assert REQUEST_RE.search(empty_fields["url"])["request"] == "Send"

        # 실패 결말이 사이클을 제대로 닫는지 - 유령 timeout과 비유령 timeout은 경로가 다르다.
        ghost_timeout_log = [
            "2026-09-19T08:00:00.000000  tag_user_data : G3P3",
            "2026-09-19T08:00:00.320000  [RFID] G3P3 is_open=0 role=ghost",
            "2026-09-19T08:00:00.560000  [RFID] Situation send G3P3 result=OK took=240ms",
            "2026-09-19T08:00:15.600000  [GhostTiming] TIMEOUT waiting for open (15000ms)",
            "2026-09-19T08:00:15.601000  [Approval] wait ended: timeout",
        ]
        # v54+ 포맷: role= 뒤에 "(Receive took=Xms)"가 붙는다. role 파싱이 접미사를
        # 삼키지 않는지, relay_on 없이도 role_receive_ms가 채워지는지 같이 검증한다.
        nonghost_log = [
            "2026-09-19T08:01:00.000000  tag_user_data : G4P4",
            "2026-09-19T08:01:00.320000  [RFID] G4P4 is_open=0 role=revival (Receive took=320ms)",
            "2026-09-19T08:01:00.330000  [GhostTiming] skip - role=revival (not ghost, open not expected)",
            "2026-09-19T08:01:00.570000  [RFID] Situation send G4P4 result=OK took=240ms",
            "2026-09-19T08:01:00.860000  [Approval] wait ended: role not eligible",
        ]
        for log_lines, expected in ((ghost_timeout_log, "approval timeout"), (nonghost_log, "role not eligible")):
            handle2 = tempfile.NamedTemporaryFile("w", suffix=".log", delete=False, encoding="utf-8")
            handle2.write("\n".join(log_lines) + "\n")
            handle2.close()
            try:
                sub_entries, _, _ = parse(handle2.name)
                sub_cycles = find_cycles(sub_entries)
                assert len(sub_cycles) == 1, sub_cycles
                if log_lines is nonghost_log:
                    role_entry = next(e for e in sub_cycles[0]["entries"] if e.kind == "role_known")
                    assert role_entry.fields["role"] == "revival", role_entry.fields
                    assert role_entry.fields["receive_ms"] == "320", role_entry.fields
                    nonghost_info = analyse_cycle(sub_cycles[0], DEFAULT_GAP_MS)
                    assert nonghost_info["role_receive_ms"] == 320, nonghost_info["role_receive_ms"]
                assert expected in sub_cycles[0]["outcome"], sub_cycles[0]["outcome"]
            finally:
                os.unlink(handle2.name)
    finally:
        os.unlink(handle.name)
    print("selftest OK")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture", nargs="?", help="capture_console.py가 만든 로그 파일")
    parser.add_argument("--gap-ms", type=float, default=DEFAULT_GAP_MS,
                        help=f"침묵 구간으로 볼 최소 간격 (기본 {DEFAULT_GAP_MS}ms)")
    parser.add_argument("--timeline", action="store_true", help="사이클마다 전체 타임라인을 출력")
    parser.add_argument("--selftest", action="store_true", help="내장 합성 로그로 파서를 검증")
    args = parser.parse_args()

    if args.selftest:
        return selftest()
    if not args.capture:
        parser.error("캡처 파일을 지정하거나 --selftest 를 쓰라")

    entries, skipped, coarse = parse(args.capture)
    if not entries:
        print("파싱된 줄이 없다. capture_console.py가 만든 파일이 맞는지 확인하라.", file=sys.stderr)
        return 1
    if skipped:
        print(f"(타임스탬프 형식이 아닌 {skipped}줄은 건너뛰었다)\n")

    if coarse:
        print("[주의] 아두이노 시리얼 모니터 형식(초 단위)이다. 기기가 스스로 잰 값"
              "([GhostTiming] RELAY ON의 total/role_receive/situation)은 그대로 신뢰할 수\n"
              "       있지만, 줄 사이 간격으로 복원하는 침묵 구간 분석은 해상도가 모자라\n"
              "       의미가 없다. 그 부분까지 보려면 capture_console.py로 다시 캡처하라.\n")

    unknown = sum(1 for e in entries if e.kind == "other")
    print(f"총 {len(entries):,}줄, 미분류 {unknown:,}줄 "
          f"({entries[0].ts.isoformat(timespec='seconds')} ~ {entries[-1].ts.isoformat(timespec='seconds')})")
    report(split_trials(entries), args.gap_ms, args.timeline)
    return 0


if __name__ == "__main__":
    sys.exit(main())
