#!/usr/bin/env python3
"""Offline protocol/measurement tests. Never opens a hardware port."""
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import rfid_bench as bench


def record(event, **fields):
    if event == "ack":
        fields.setdefault("ok", True)
    return {"protocol": 1, "event": event, **fields}


def wire(*records):
    return b"".join((bench.PREFIX + json.dumps(r) + "\n").encode() for r in records)


def trial(number, uid=True, payload=True, gxpx=True, elapsed=1000):
    return record("trial", scan_id=1, id=number, uid_ok=uid, ok=payload,
                  valid_gxpx=gxpx, elapsed_us=elapsed, stages=[])


class FakeSerial:
    def __init__(self, data=b""):
        self.data = data
        self.writes = []
        self.closed = False

    def read(self, count):
        chunk, self.data = self.data[:count], self.data[count:]
        return chunk

    def write(self, data):
        self.writes.append(data)
        return len(data)

    def close(self):
        self.closed = True


class FakeClock:
    def __init__(self):
        self.value = 0

    def __call__(self):
        self.value += 0.001
        return self.value


class BenchTests(unittest.TestCase):
    def session(self, data=b""):
        artifact = bench.Artifact(io.StringIO())
        return bench.Session(FakeSerial(data), artifact, clock=FakeClock())

    def rows(self, *records, requested=4):
        return [{"kind": "metadata", "config": {"trials": requested}}] + [
            {"kind": "rx", "record": r, "host_monotonic_ns": 999999999999} for r in records]

    def test_parser_ignores_boot_and_checks_protocol(self):
        self.assertIsNone(bench.parse_record("ESP-ROM:esp32"))
        self.assertEqual(bench.parse_record(bench.PREFIX + '{"protocol":1,"event":"ready"}')["event"], "ready")
        for invalid in ("{", "[]", '{"event":"ready"}', '{"protocol":2,"event":"ready"}', '{"protocol":1}'):
            with self.assertRaises(bench.BenchError):
                bench.parse_record(bench.PREFIX + invalid)

    def test_cli_timeout_matches_bounded_firmware_contract(self):
        arguments = ['scan', '--port', 'FAKE', '--out', 'unused.jsonl',
                     '--card', 'test', '--reader', 'test', '--orientation', 'parallel']
        for value in ('0', '251', '2000'):
            with patch.object(bench, 'capture') as capture, patch('sys.stderr', io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    bench.main(arguments + ['--timeout-ms', value])
                self.assertEqual(error.exception.code, 2)
                capture.assert_not_called()
        for value in ('1', '250'):
            with patch.object(bench, 'capture', return_value={}) as capture, patch('sys.stdout', io.StringIO()):
                self.assertEqual(bench.main(arguments + ['--timeout-ms', value]), 0)
                self.assertEqual(capture.call_args.args[0].timeout_ms, int(value))

    def test_counts_failures_invalid_payloads_and_missing(self):
        summary = bench.analyze(self.rows(
            trial(1, elapsed=1000), trial(2, uid=False, payload=False, gxpx=False, elapsed=250000),
            trial(3, payload=False, gxpx=False), trial(4, gxpx=False), requested=5))
        self.assertEqual(summary["missing"], 1)
        self.assertEqual(summary["uid_failures"], 1)
        self.assertEqual(summary["payload_failures_after_uid"], 1)
        self.assertEqual(summary["invalid_payloads"], 1)
        self.assertEqual(summary["payload_success_rate_of_requested"], 0.4)
        self.assertEqual(summary["gxpx_success_rate_of_requested"], 0.2)
        self.assertEqual(summary["failed_scan_latency_ms"]["max"], 250)

    def test_success_latency_uses_only_mcu_successes(self):
        summary = bench.analyze(self.rows(trial(1, elapsed=1000), trial(2, elapsed=3000),
                                          trial(3, uid=False, payload=False, gxpx=False, elapsed=999000)))
        self.assertEqual(summary["payload_success_scan_latency_ms"],
                         {"n": 2, "p50": 2.0, "p95": 2.9, "max": 3.0})

    def test_empty_scan_reports_no_success_latency(self):
        summary = bench.analyze(self.rows())
        self.assertEqual(summary["missing"], 4)
        self.assertIsNone(summary["payload_success_scan_latency_ms"]["p95"])

    def test_duplicates_do_not_inflate_success(self):
        sample = trial(1)
        summary = bench.analyze(self.rows(sample, sample))
        self.assertEqual(summary["payload_successes"], 1)
        self.assertIn("duplicate", summary["warnings"][-1])
        with self.assertRaises(bench.BenchError):
            bench.analyze(self.rows(sample, trial(1, elapsed=2)))

    def test_mixed_sessions_and_invalid_trials_rejected(self):
        wrong_scan = trial(2)
        wrong_scan["scan_id"] = 2
        with self.assertRaises(bench.BenchError):
            bench.analyze(self.rows(trial(1), wrong_scan))
        for change in ({"elapsed_us": -1}, {"uid_ok": False}, {"ok": False}, {"id": "1"}):
            sample = trial(1)
            sample.update(change)
            with self.assertRaises(bench.BenchError):
                bench.analyze(self.rows(sample))

    def test_stage_failures_include_recovered_trials(self):
        sample = trial(1)
        sample["stages"] = [{"op": "uid", "ok": False}, {"op": "uid", "ok": True}]
        summary = bench.analyze(self.rows(sample))
        self.assertEqual(summary["failed_stages_including_recovered_trials"], {"uid": 1})

    def test_stage_overflow_is_visible(self):
        sample = trial(1)
        sample["stages_overflow"] = True
        self.assertIn("incomplete", bench.analyze(self.rows(sample))["warnings"][-1])

    def test_receive_preserves_raw_logs_and_ack(self):
        session = self.session(b"boot log\r\n" + wire(record("ack", cmd="gain")))
        self.assertEqual(session.command("gain 18", 1)["cmd"], "gain")
        self.assertEqual(session.port.writes, [b"gain 18\n"])
        self.assertEqual(session.artifact.rows[1]["raw_hex"], b"boot log\r\n".hex())

    def test_device_errors_and_timeouts_are_bounded(self):
        session = self.session(wire(record("error", cmd="scan", reason="busy")))
        with self.assertRaisesRegex(bench.BenchError, "busy"):
            session.command("scan 20", 1)
        with self.assertRaisesRegex(bench.BenchError, "Timed out"):
            self.session().command("status", 0.02)

    def test_failed_or_missing_ok_ack_rejected(self):
        for cmd in ("gain", "retries", "reset"):
            for ack in (record("ack", cmd=cmd, ok=False), {"protocol": 1, "event": "ack", "cmd": cmd}):
                with self.assertRaisesRegex(bench.BenchError, "could not apply"):
                    self.session(wire(ack)).command(cmd, 1)

    def test_failed_reset_never_starts_scan(self):
        fake = FakeSerial(wire(record("status", running=False, pn532_ready=False),
                               record("ack", cmd="reset", ok=False, pn532_ready=False)))
        with tempfile.TemporaryDirectory() as directory:
            args = SimpleNamespace(out=Path(directory) / "failed.jsonl", port="FAKE", gain="18",
                                   timeout_ms=250, retries=10, trials=1, interval_ms=300, deadline_s=1,
                                   baud=115200, card="none", reader="test-reader", distance_mm=None,
                                   orientation="unknown", note="", reset=True, command_timeout_s=1)
            with patch.object(bench, "open_port", return_value=fake), patch.object(bench, "git_revision", return_value={}):
                with self.assertRaisesRegex(bench.BenchError, "could not apply reset"):
                    bench.capture(args)
            self.assertTrue(fake.closed)
            self.assertEqual(fake.writes, [b"status\n", b"reset\n"])

    def test_reset_after_handshake_aborts(self):
        for data in (wire(record("ready")), b"ESP-ROM:esp32\n", b"rst:0x1 (POWERON_RESET)\n"):
            session = self.session(data)
            session.handshaken = True
            with self.assertRaisesRegex(bench.BenchError, "rebooted"):
                session.receive(1)

    def test_overlong_line_stops(self):
        session = self.session(b"x" * (bench.MAX_LINE_BYTES + 1))
        with self.assertRaisesRegex(bench.BenchError, "64 KiB"):
            session.receive(1)

    def test_capture_open_once_records_labels_and_closes(self):
        fake = FakeSerial(wire(record("ready"), record("status", running=False, pn532_ready=True),
                               record("ack", cmd="gain"), record("ack", cmd="timeout"),
                               record("ack", cmd="retries"), record("ack", cmd="scan", scan_id=1),
                               trial(1), record("done", scan_id=1, requested=1, count=1, reason="complete")))
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "capture.jsonl"
            args = SimpleNamespace(out=output, port="FAKE", gain="18", timeout_ms=250, retries=10,
                                   trials=1, interval_ms=300, deadline_s=1, baud=115200,
                                   card="test-card", reader="test-reader", distance_mm=10,
                                   orientation="parallel", note="", reset=False, command_timeout_s=1)
            with patch.object(bench, "open_port", return_value=fake) as opened, \
                    patch.object(bench, "git_revision", return_value={"sha": "fixture", "dirty": False}):
                summary = bench.capture(args)
            opened.assert_called_once_with("FAKE", 115200)
            self.assertTrue(fake.closed)
            self.assertEqual(summary["observed"], 1)
            self.assertEqual(summary["labels"]["distance_mm"], 10)
            self.assertEqual(summary["git"]["sha"], "fixture")
            self.assertNotIn(b"reset\n", fake.writes)
            self.assertEqual(fake.writes.count(b"status\n"), 2)
            self.assertEqual(json.loads(output.read_text().splitlines()[-1])["kind"], "host_done")
            with self.assertRaises(FileExistsError):
                bench.capture(args)


if __name__ == "__main__":
    unittest.main()
