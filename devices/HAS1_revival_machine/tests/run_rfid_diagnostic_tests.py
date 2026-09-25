#!/usr/bin/env python3
"""Exercise the actual low-level RFID functions and diagnostic serial parser.

This verifies command sequencing/instrumentation only. A fake PN532 cannot
measure antenna coupling, detection distance or real transport timeouts.
"""
from pathlib import Path
import json
import os
import subprocess
import tempfile
from sensor_test_support import write_sensor_types

DEVICE = Path(__file__).resolve().parents[1]


def main() -> None:
    sensor = (DEVICE / "sensor.ino").read_text()
    low_level = sensor[sensor.index("static GainMode currentGain"):
                       sensor.index("/**\n * @brief RFID(=PN532) 세팅")]
    diagnostics = (DEVICE / "rfid_diagnostics.ino").read_text().replace(
        '#include "HAS1_revival_machine.h"', "", 1)
    with tempfile.TemporaryDirectory(prefix="revival-rfid-diagnostic-tests-") as directory:
        build = Path(directory)
        write_sensor_types(build)
        (build / "low_level.inc").write_text(low_level)
        (build / "diagnostics.inc").write_text(diagnostics)
        binary = build / "diagnostics_test"
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-I", str(build), "-I", str(DEVICE / "tests" / "fakes"), "-I", str(DEVICE),
                        str(DEVICE / "tests" / "rfid_diagnostic_tests.cpp"), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], check=True, text=True, capture_output=True)
    records = []
    for line in result.stdout.splitlines():
        assert line.startswith("[RFID_DIAG] "), line
        record = json.loads(line[len("[RFID_DIAG] "):])
        assert record["protocol"] == 1
        records.append(record)
    trials = [r for r in records if r["event"] == "trial"]
    assert trials and all(not r["hardware_gain_confirmed"] for r in trials)
    assert all(not r["stages_overflow"] for r in trials)
    assert any(r["ok"] and r["valid_gxpx"] and r["payload_hex"] == "47395033" for r in trials)
    assert any(r["uid_ok"] and not r["ok"] and r["payload_hex"] == "" for r in trials)
    assert any(s["uid_hex"] == "31323334353637383930" for r in trials for s in r["stages"] if s["op"] == "uid" and s["ok"])
    auto = next(r for r in trials if r["mode"] == "auto")
    assert [s["gain_db"] for s in auto["stages"] if s["op"] == "uid"] == [23, 18, 33]
    assert [s["op"] for s in auto["stages"]][-2:] == ["rf_off", "rf_on"]
    assert any(r["event"] == "ack" and r["cmd"] == "gain" and not r["ok"] for r in records)
    assert any(r["event"] == "done" and r["reason"] == "stopped" and r["count"] == 0 for r in records)
    assert any(r["event"] == "error" and r["reason"] == "line_too_long_or_invalid" for r in records)
    print(f"PASS: diagnostic shared RFID paths, command bounds, stop, clock wrap and {len(records)} JSON records")


if __name__ == "__main__":
    main()
