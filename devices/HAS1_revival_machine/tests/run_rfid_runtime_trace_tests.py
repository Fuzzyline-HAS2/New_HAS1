#!/usr/bin/env python3
"""Verify production RFID trace against the actual instrumented sensor functions.

Compiles trace disabled/enabled independently and compares every PN532 call,
argument, result and returned payload. A fake PN532 verifies instrumentation,
not RF range, coupling, hardware timing or readiness semantics.
"""
from pathlib import Path
import json
import os
import subprocess
import tempfile
from sensor_test_support import write_sensor_types
from run_host_tests import extract_functions

DEVICE = Path(__file__).resolve().parents[1]
CASES = (
    "startup", "success", "uid_failure", "page_failure", "gain_switch", "gain_ack_failure",
    "fixed_uid_failure", "admin_payload", "gap", "clock_wrap", "two_scans",
    "stage_overflow", "emit_delay", "fast_empty", "slow_empty", "post_read",
    "actual_gameplay", "actual_ready", "actual_ready_nonadmin", "actual_pending",
    "actual_debounce", "actual_pending_skip", "actual_pending_recent", "actual_pending_fault",
)


def execute(binary: Path, scenario: str) -> tuple[dict, list[dict]]:
    result = subprocess.run([str(binary), scenario], check=True, text=True, capture_output=True)
    lines = result.stdout.splitlines()
    assert lines and lines[0].startswith("[TEST_RESULT] "), result.stdout
    transcript = json.loads(lines[0][len("[TEST_RESULT] "):])
    records = []
    for line in lines[1:]:
        assert line.startswith("[RFID_TRACE] "), line
        records.append(json.loads(line[len("[RFID_TRACE] "):]))
    return transcript, records


def main() -> None:
    sensor = (DEVICE / "sensor.ino").read_text()
    low_level = sensor[sensor.index("static GainMode currentGain"):
                       sensor.index("/**\n * @brief RFID(=PN532) 세팅")]
    runtime = (DEVICE / "rfid_runtime_trace.ino").read_text().replace(
        '#include "HAS1_revival_machine.h"', "", 1)
    with tempfile.TemporaryDirectory(prefix="revival-rfid-runtime-tests-") as directory:
        build = Path(directory)
        write_sensor_types(build)
        (build / "low_level.inc").write_text(low_level)
        (build / "runtime_trace.inc").write_text(runtime)
        (build / "scan_callers.inc").write_text(extract_functions(
            DEVICE / "sensor.ino", {"RfidLoop", "AdminCardPollPending", "AdminCardPollReady"}))
        (build / "main_loop.inc").write_text(extract_functions(
            DEVICE / "HAS1_revival_machine.ino", {"loop"}))
        verify_config(build)
        binaries = []
        for enabled in (0, 1):
            binary = build / f"runtime_trace_{enabled}"
            subprocess.run([
                os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                f"-DREVIVAL_RFID_RUNTIME_TRACE={enabled}",
                "-I", str(build), "-I", str(DEVICE / "tests" / "fakes"), "-I", str(DEVICE),
                str(DEVICE / "tests" / "rfid_runtime_trace_tests.cpp"), "-o", str(binary),
            ], check=True)
            binaries.append(binary)
        all_records = {}
        for scenario in CASES:
            baseline, silent = execute(binaries[0], scenario)
            traced, records = execute(binaries[1], scenario)
            assert not silent, (scenario, silent)
            assert baseline == traced, (scenario, baseline, traced)
            verify_reader_calls(scenario, baseline)
            assert bool(records) == (scenario != "fast_empty"), scenario
            all_records[scenario] = records
    verify_records(all_records)
    print(f"PASS: {len(CASES)} runtime trace cases; trace-off/on PN532 command/result parity")


def verify_config(build: Path) -> None:
    header = (DEVICE / "HAS1_revival_machine.h").read_text()
    config = header[header.index("#ifndef REVIVAL_RFID_DIAGNOSTICS"):
                    header.index("// Telnet 원격 디버깅 콘솔")]
    source = build / "config_test.cpp"
    source.write_text(
        "#include <cstdint>\n" + config +
        "\nstatic_assert(REVIVAL_RFID_RUNTIME_TRACE == EXPECT_TRACE);\n"
        "static_assert(REVIVAL_RFID_DIAGNOSTICS == EXPECT_DIAGNOSTIC);\n")
    command = [os.environ.get("CXX", "c++"), "-std=c++17", "-fsyntax-only",
               "-I", str(DEVICE), str(source)]
    for trace, diagnostic in ((0, 0), (1, 0), (0, 1), (1, 1)):
        defines = [f"-DEXPECT_TRACE={trace}", f"-DEXPECT_DIAGNOSTIC={diagnostic}"]
        # The 0/0 build deliberately supplies neither flag, checking actual
        # production defaults rather than redefining them in the test harness.
        if trace:
            defines += ["-DREVIVAL_RFID_RUNTIME_TRACE=1"]
        if diagnostic:
            defines += ["-DREVIVAL_RFID_DIAGNOSTICS=1"]
        result = subprocess.run(command + defines, text=True, capture_output=True)
        if trace and diagnostic:
            assert result.returncode != 0, "Both incompatible firmware modes compiled"
        else:
            assert result.returncode == 0, result.stderr


def verify_reader_calls(scenario: str, transcript: dict) -> None:
    uid = [2, 0, 250]
    page = [3, 7]

    def gain(config: int) -> list[int]:
        return [1, 100, 0x32, 0x0A, config, 0xF4, 0x3F, 0x11, 0x4D,
                0x85, 0x61, 0x6F, 0x26, 0x62, 0x87]

    reset = [[1, 100, 0x32, 1, 0], [1, 100, 0x32, 1, 1]]
    expected = {
        "startup": [],
        "success": [uid, page],
        "admin_payload": [uid, page],
        "clock_wrap": [uid, page],
        "post_read": [uid, page],
        "gap": [uid, page, uid, page],
        "two_scans": [uid, page, uid, page],
        "emit_delay": [uid, page, uid, page],
        "fixed_uid_failure": [uid],
        "gain_switch": [uid, gain(0x09), uid, page],
        "gain_ack_failure": [uid, gain(0x09)],
        # Only validated no-target responses continue the unique-gain sweep.
        "uid_failure": [uid, gain(0x09), uid, gain(0x49), uid] + reset,
        "page_failure": [uid, page],
        "stage_overflow": [],
        "fast_empty": [],
        "slow_empty": [],
    }
    if scenario.startswith("actual_"):
        expected[scenario] = [] if scenario in ("actual_debounce", "actual_pending_skip", "actual_pending_recent", "actual_pending_fault") else [uid, page]
    assert transcript["operations"] == expected[scenario], transcript


def verify_records(records: dict[str, list[dict]]) -> None:
    # The JSON assertions are deliberately separate from firmware implementation
    # so field names, units and event boundaries form an executable contract.
    for scenario, events in records.items():
        for event in events:
            assert event["v"] == 1 and event["fw"] == 67, (scenario, event)
            assert event["runtime_trace"] is True
            assert event["game_state"] == event["device_state"] == "activate"
            if event["event"] == "loop":
                assert event["prev_emit_bytes"] > 0  # Startup emitted first.
                assert event["dropped"] == (1 if scenario == "two_scans" else 0)
                assert event["stage_dropped"] == (2 if scenario == "stage_overflow" else 0)

    startup, = records["startup"]
    assert startup["event"] == "startup" and startup["diagnostics"] is False
    assert startup["uid_timeout_ms"] == 250 and startup["rf_config_timeout_ms"] == 100
    assert startup["retries"] == 10 and startup["slow_loop_us"] == 40000

    def scan(scenario: str, index: int = 0) -> dict:
        event = records[scenario][index]
        assert event["event"] == "loop"
        result = event["scan"]
        assert result is not None
        return result

    success = scan("success")
    assert success["ctx"] == "gameplay" and success["ok"] is True
    assert success["us"] == 150 and success["gap_us"] is None
    assert success["payload_hex"] == "47395033"
    assert success["stages"] == [["uid", 0x19, 0, 100, 1, ""],
                                  ["page7", 0x19, 100, 50, 1, "47395033"]]
    assert scan("admin_payload")["payload_hex"] == "4D4D4D4D"

    failed = scan("uid_failure")
    assert failed["ok"] is False and failed["payload_hex"] == "" and failed["us"] == 12420
    assert [s[0] for s in failed["stages"]] == [
        "uid", "gain", "uid", "gain", "uid", "rf_off", "rf_on"]
    assert [s[1] for s in failed["stages"] if s[0] == "uid"] == [0x19, 0x09, 0x49]
    assert all(s[4] == 0 and s[5] == "" for s in failed["stages"] if s[0] == "uid")
    assert all(s[3] == 30 and s[4] == 1 for s in failed["stages"] if s[0] != "uid")

    failed = scan("page_failure")
    assert failed["ok"] is False and failed["payload_hex"] == "" and failed["us"] == 150
    assert len(failed["stages"]) == 2
    assert all(s[4] == 1 and s[5] == "" for s in failed["stages"] if s[0] == "uid")
    assert all(s[3] == 50 and s[4] == 0 and s[5] == "" for s in failed["stages"] if s[0] == "page7")

    for scenario in ("gain_switch", "gain_ack_failure"):
        switched = scan(scenario)
        assert switched["ok"] == (scenario == "gain_switch")
        assert switched["us"] == (280 if scenario == "gain_switch" else 130)
        assert [s[0] for s in switched["stages"]] == (["uid", "gain", "uid", "page7"] if scenario == "gain_switch" else ["uid", "gain"])
        assert switched["stages"][1] == ["gain", 0x09, 100, 30, int(scenario == "gain_switch"), ""]
    fixed = scan("fixed_uid_failure")
    assert fixed["ctx"] == "admin_pending" and fixed["us"] == 100
    assert fixed["stages"] == [["uid", 0x19, 0, 100, 0, ""]]

    first, second = records["gap"]
    assert first["scan"]["gap_us"] is None
    assert second["scan"]["gap_us"] == 1560000 and second["scan"]["us"] == 150
    assert [second[k] for k in ("telnet_us", "timer_us", "neo_us", "rfid_game_us")] == [10000, 20000, 30000, 150]
    assert second["loop_us"] == 60150 and second["post_read_us"] == 0

    wrapped, = records["clock_wrap"]
    assert wrapped["loop_start_us"] == 2 ** 32 - 51
    assert wrapped["loop_us"] == 150 and wrapped["post_read_us"] == 0
    assert wrapped["scan"]["us"] == 150 and wrapped["scan"]["stages"] == success["stages"]

    extra, = records["two_scans"]
    assert extra["loop_us"] == 300 and extra["post_read_us"] == 150
    assert extra["scan"]["stages"] == success["stages"]
    overflow = scan("stage_overflow")
    assert len(overflow["stages"]) == 16 and overflow["us"] == 18
    assert all(s == ["uid", 0x19, i, 1, 0, ""] for i, s in enumerate(overflow["stages"]))

    first, second = records["emit_delay"]
    assert first["prev_emit_us"] == 0 and second["prev_emit_us"] == 7000
    assert second["scan"]["gap_us"] == 7000
    assert second["scan"]["us"] == 150 and second["loop_us"] == 150
    assert records["fast_empty"] == []
    slow, = records["slow_empty"]
    assert slow["loop_us"] == slow["timer_us"] == 40000
    assert slow["scan"] is None and slow["post_read_us"] is None
    delayed, = records["post_read"]
    assert delayed["post_read_us"] == 2300000 and delayed["loop_us"] == 2300150
    assert delayed["scan"]["us"] == 150

    for scenario, context in (("actual_gameplay", "gameplay"), ("actual_ready", "admin_ready"),
                              ("actual_ready_nonadmin", "admin_ready"), ("actual_pending", "admin_pending")):
        event, = records[scenario]
        captured = event["scan"]
        assert captured["ctx"] == context and captured["ok"] is True
        assert captured["us"] == 150 and captured["gap_us"] is None
        assert [event[k] for k in ("telnet_us", "timer_us", "neo_us")] == [10000, 20000, 30000]
        post_read = 2300000 if scenario == "actual_gameplay" else 0
        assert event["post_read_us"] == post_read
        assert event["rfid_game_us"] == 150 + post_read
        assert event["loop_us"] == 60150 + post_read
        assert captured["payload_hex"] == ("4D4D4D4D" if scenario in ("actual_ready", "actual_pending") else "47395033")
    for scenario in ("actual_debounce", "actual_pending_skip", "actual_pending_recent", "actual_pending_fault"):
        event, = records[scenario]
        assert event["scan"] is None and event["post_read_us"] is None
        assert event["rfid_game_us"] == 0 and event["loop_us"] == 60000


if __name__ == "__main__":
    main()
