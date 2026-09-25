#!/usr/bin/env python3
"""Bounded USB capture and offline analysis for the Revival PN532 bench build."""
from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time

PREFIX = "[RFID_DIAG] "
MAX_LINE_BYTES = 65536


class BenchError(Exception):
    pass


def parse_record(line: str):
    """Ignore ordinary boot logs; reject malformed diagnostic records."""
    if not line.startswith(PREFIX):
        return None
    try:
        record = json.loads(line[len(PREFIX):])
    except json.JSONDecodeError as exc:
        raise BenchError(f"Malformed diagnostic JSON: {exc}") from exc
    if not isinstance(record, dict) or record.get("protocol") != 1:
        raise BenchError("Unsupported RFID diagnostic protocol (expected protocol=1)")
    if not isinstance(record.get("event"), str):
        raise BenchError("Diagnostic record has no event name")
    return record


def percentile(values, fraction):
    if not values:
        return None
    values = sorted(values)
    position = (len(values) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    return values[lower] + (values[upper] - values[lower]) * (position - lower)


def latency_ms(values):
    values = [value / 1000.0 for value in values]
    return {"n": len(values), "p50": percentile(values, 0.5),
            "p95": percentile(values, 0.95), "max": max(values) if values else None}


def analyze(rows):
    """Use MCU durations only. Missing trials are never counted as successes."""
    metadata = next((row for row in rows if row.get("kind") == "metadata"), {})
    requested = metadata.get("config", {}).get("trials", 0)
    records = [row["record"] for row in rows if isinstance(row.get("record"), dict)]
    scan_acks = [r for r in records if r.get("event") == "ack" and r.get("cmd") == "scan"]
    scan_id = scan_acks[0].get("scan_id") if scan_acks else None
    trials = {}
    duplicate_count = 0
    for record in records:
        if record.get("event") != "trial":
            continue
        if scan_id is None:
            scan_id = record.get("scan_id")
        if record.get("scan_id") != scan_id:
            raise BenchError("Artifact contains trials from multiple scan sessions")
        trial_id = record.get("id")
        duration = record.get("elapsed_us")
        if type(trial_id) is not int or type(duration) is not int or duration < 0:
            raise BenchError("Trial is missing its integer id or nonnegative elapsed_us")
        if not all(type(record.get(key)) is bool for key in ("ok", "uid_ok", "valid_gxpx")):
            raise BenchError("Trial is missing boolean result fields")
        if record["ok"] and not record["uid_ok"]:
            raise BenchError("Trial reports a payload without UID detection")
        if record["valid_gxpx"] and not record["ok"]:
            raise BenchError("Trial reports GxPx without a payload read")
        if trial_id in trials:
            if trials[trial_id] != record:
                raise BenchError(f"Conflicting duplicate trial {trial_id}")
            duplicate_count += 1
        trials[trial_id] = record
    done = next((r for r in reversed(records)
                 if r.get("event") == "done" and r.get("scan_id") == scan_id), None)
    if not requested and done:
        requested = done.get("requested", 0)
    if type(requested) is not int or requested < 0:
        raise BenchError("Invalid requested trial count")
    observed = list(trials.values())
    uid_success = sum(r["uid_ok"] for r in observed)
    payload_success = sum(r["ok"] for r in observed)
    gxpx_success = sum(r["valid_gxpx"] for r in observed)
    stage_failures = Counter()
    for record in observed:
        for stage in record.get("stages", []):
            if stage.get("ok") is False:
                stage_failures[stage.get("op", "unknown")] += 1
    warnings = []
    if not done:
        warnings.append("No done event: capture is incomplete.")
    elif done.get("count") != len(observed):
        warnings.append("Device done count differs from captured unique trials.")
    if len(observed) > requested:
        warnings.append("Captured more trials than requested.")
    if duplicate_count:
        warnings.append(f"Ignored {duplicate_count} duplicate trial records.")
    if any(record.get("stages_overflow") for record in records):
        warnings.append("MCU stage buffer overflowed: stage-level evidence is incomplete.")
    return {
        "labels": metadata.get("labels", {}), "git": metadata.get("git", {}),
        "config": metadata.get("config", {}), "scan_id": scan_id,
        "requested": requested, "observed": len(observed),
        "missing": max(0, requested - len(observed)),
        "uid_successes": uid_success, "payload_successes": payload_success,
        "gxpx_successes": gxpx_success,
        "uid_failures": len(observed) - uid_success,
        "payload_failures_after_uid": uid_success - payload_success,
        "invalid_payloads": payload_success - gxpx_success,
        "payload_success_rate_of_requested": payload_success / requested if requested else None,
        "gxpx_success_rate_of_requested": gxpx_success / requested if requested else None,
        "failed_stages_including_recovered_trials": dict(stage_failures),
        "payload_success_scan_latency_ms": latency_ms([r["elapsed_us"] for r in observed if r["ok"]]),
        "gxpx_success_scan_latency_ms": latency_ms([r["elapsed_us"] for r in observed if r["valid_gxpx"]]),
        "failed_scan_latency_ms": latency_ms([r["elapsed_us"] for r in observed if not r["ok"]]),
        "done_reason": done.get("reason") if done else None, "warnings": warnings,
        "timing_scope": "MCU scan start through completed scan; not physical card presentation-to-read time.",
    }


class Artifact:
    def __init__(self, output):
        self.output = output
        self.rows = []

    def emit(self, kind, **fields):
        row = {"kind": kind, "host_monotonic_ns": time.monotonic_ns(),
               "host_utc": datetime.now(timezone.utc).isoformat(), **fields}
        self.rows.append(row)
        self.output.write(json.dumps(row, ensure_ascii=False) + "\n")
        self.output.flush()


class Session:
    def __init__(self, port, artifact, clock=time.monotonic):
        self.port = port
        self.artifact = artifact
        self.clock = clock
        self.pending = bytearray()
        self.handshaken = False

    def send(self, command):
        payload = (command + "\n").encode("ascii")
        self.artifact.emit("tx", command=command)
        if self.port.write(payload) != len(payload):
            raise BenchError("Incomplete serial write")
        # No flush(): some drivers can block in flush despite write_timeout.

    def receive(self, deadline):
        while self.clock() < deadline:
            newline = self.pending.find(b"\n")
            if newline < 0:
                chunk = self.port.read(1024)
                if chunk:
                    self.pending.extend(chunk)
                if len(self.pending) > MAX_LINE_BYTES:
                    self.artifact.emit("rx_overflow", raw_hex=self.pending.hex())
                    raise BenchError("Serial line exceeds 64 KiB")
                continue
            raw = bytes(self.pending[:newline + 1])
            del self.pending[:newline + 1]
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            try:
                record = parse_record(line)
            except BenchError:
                self.artifact.emit("rx", raw=line, raw_hex=raw.hex(), record=None)
                raise
            self.artifact.emit("rx", raw=line, raw_hex=raw.hex(), record=record)
            if self.handshaken and ((record and record["event"] == "ready")
                                    or line.startswith(("rst:0x", "ESP-ROM:"))):
                raise BenchError("Board rebooted after handshake; scan not replayed")
            if record:
                if record["event"] == "error":
                    raise BenchError(f"Device rejected {record.get('cmd')}: {record.get('reason')}")
                return record
        raise BenchError("Timed out waiting for device response")

    def wait(self, predicate, deadline):
        while True:
            record = self.receive(deadline)
            if predicate(record):
                return record

    def command(self, command, timeout):
        self.send(command)
        name = command.split()[0]
        ack = self.wait(lambda r: r["event"] == "ack" and r.get("cmd") == name,
                        self.clock() + timeout)
        if ack.get("ok") is not True:
            raise BenchError(f"Device could not apply {name}; inspect recorded stage results")
        return ack


def git_revision():
    directory = Path(__file__).resolve().parent
    try:
        sha = subprocess.run(["git", "rev-parse", "HEAD"], cwd=directory,
                             capture_output=True, text=True, check=True, timeout=3).stdout.strip()
        dirty = subprocess.run(["git", "status", "--porcelain"], cwd=directory,
                               capture_output=True, text=True, check=True, timeout=3).stdout != ""
        return {"sha": sha, "dirty": dirty}
    except (OSError, subprocess.SubprocessError):
        return {"sha": None, "dirty": None}


def serial_module():
    try:
        import serial
        return serial
    except ImportError as exc:
        raise BenchError("pyserial is required for hardware access; install it in a virtual environment.") from exc


def open_port(name, baud):
    serial = serial_module()
    # Set control lines before opening; drivers may still pulse them at open.
    port = serial.Serial(port=None, baudrate=baud, timeout=0.1, write_timeout=1,
                         rtscts=False, dsrdtr=False, **({"exclusive": True} if os.name == "posix" else {}))
    port.dtr = False
    port.rts = False
    port.port = name
    port.open()
    return port


def capture(args):
    config = {key: getattr(args, key) for key in
              ("gain", "timeout_ms", "retries", "trials", "interval_ms", "deadline_s", "baud")}
    labels = {key: getattr(args, key) for key in ("card", "reader", "distance_mm", "orientation", "note")}
    # Exclusive create preserves earlier measurements, including aborted runs.
    with Path(args.out).open("x", encoding="utf-8") as output:
        artifact = Artifact(output)
        artifact.emit("metadata", schema=1, config=config, labels=labels, git=git_revision(), port=args.port)
        port = None
        session = None
        running = False
        try:
            port = open_port(args.port, args.baud)
            session = Session(port, artifact)
            session.send("status")
            handshake_deadline = time.monotonic() + args.command_timeout_s
            while True:
                status = session.receive(handshake_deadline)
                if status["event"] == "status":
                    break
                if status["event"] == "ready":
                    # An open-time reset may have discarded the first status command.
                    session.send("status")
            session.handshaken = True
            if status.get("running"):
                raise BenchError("A scan is already running; no configuration or stop command sent")
            if args.reset:
                session.command("reset", args.command_timeout_s)
            elif not status.get("pn532_ready"):
                raise BenchError("PN532 is not ready; inspect wiring/boot logs before retrying")
            for command in (f"gain {args.gain}", f"timeout {args.timeout_ms}", f"retries {args.retries}"):
                session.command(command, args.command_timeout_s)
            # Treat scan as potentially running even if its ack is lost.
            running = True
            ack = session.command(f"scan {args.trials} {args.interval_ms}", args.command_timeout_s)
            if type(ack.get("scan_id")) is not int:
                raise BenchError("Scan acknowledgement has no integer scan_id")
            deadline = time.monotonic() + args.deadline_s
            done = session.wait(lambda r: r["event"] == "done" and r.get("scan_id") == ack["scan_id"], deadline)
            running = False
            summary = analyze(artifact.rows)
            if done.get("reason") != "complete" or done.get("count") != args.trials or summary["missing"]:
                raise BenchError("Device stopped before all requested trials were captured")
            artifact.emit("host_done", status="complete")
            return summary
        except (Exception, KeyboardInterrupt) as exc:
            artifact.emit("host_error", reason=str(exc) or type(exc).__name__)
            if running and session:
                try:
                    session.command("stop", min(args.command_timeout_s, 3))
                except (Exception, KeyboardInterrupt) as stop_exc:
                    artifact.emit("stop_unconfirmed", reason=str(stop_exc) or type(stop_exc).__name__)
            raise
        finally:
            if session and session.pending:
                artifact.emit("rx_partial", raw_hex=session.pending.hex())
            if port:
                port.close()


def bounded_int(low, high):
    def convert(value):
        value = int(value)
        if not low <= value <= high:
            raise argparse.ArgumentTypeError(f"must be between {low} and {high}")
        return value
    return convert


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    commands.add_parser("ports", help="List USB/serial ports without opening them")
    scan = commands.add_parser("scan", help="Run one finite scan using diagnostic firmware")
    scan.add_argument("--port", required=True)
    scan.add_argument("--out", required=True, help="New JSONL path; existing files are never overwritten")
    scan.add_argument("--gain", choices=("auto", "18", "23", "33", "38"), default="auto")
    scan.add_argument("--timeout-ms", type=bounded_int(1, 250), default=250)
    scan.add_argument("--retries", type=bounded_int(0, 50), default=10)
    scan.add_argument("--trials", type=bounded_int(1, 1000), default=20)
    scan.add_argument("--interval-ms", type=bounded_int(0, 10000), default=300)
    scan.add_argument("--deadline-s", type=bounded_int(1, 3600), default=120)
    scan.add_argument("--command-timeout-s", type=bounded_int(1, 60), default=10)
    scan.add_argument("--baud", type=bounded_int(1200, 2000000), default=115200)
    scan.add_argument("--reset", action="store_true", help="Explicitly reinitialize PN532 before configuration")
    scan.add_argument("--card", required=True)
    scan.add_argument("--reader", required=True)
    scan.add_argument("--distance-mm", type=bounded_int(0, 10000), help="Omit when distance is unknown or no card is present")
    scan.add_argument("--orientation", required=True)
    scan.add_argument("--note", default="")
    analysis = commands.add_parser("analyze", help="Analyze saved JSONL without USB access or pyserial")
    analysis.add_argument("capture")
    args = parser.parse_args(argv)
    try:
        if args.action == "ports":
            serial_module()
            from serial.tools import list_ports
            result = [{"port": p.device, "description": p.description, "hwid": p.hwid}
                      for p in list_ports.comports()]
        elif args.action == "scan":
            result = capture(args)
        else:
            with Path(args.capture).open(encoding="utf-8") as source:
                result = analyze([json.loads(line) for line in source if line.strip()])
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except (BenchError, OSError, ValueError) as exc:
        print(f"RFID bench: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("RFID bench: interrupted; partial capture retained", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
