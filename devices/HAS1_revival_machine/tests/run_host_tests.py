#!/usr/bin/env python3
"""Compile real scheduler/RFID/relay/local-timing logic against deterministic host fakes.

No Arduino SDK or hardware is required. Generated includes and binaries live in
TemporaryDirectory; production .ino files are never copied into the repository.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
from sensor_test_support import write_sensor_types

TESTS = Path(__file__).resolve().parent
DEVICE = TESTS.parent


def extract_functions(source: Path, names: set[str]) -> str:
    text = source.read_text()
    # Mask strings/comments while retaining positions to match balanced braces.
    masked = re.sub(
        r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda match: re.sub(r"[^\n]", " ", match.group()),
        text,
        flags=re.S,
    )
    definitions = []
    found = set()
    for match in re.finditer(r"^(?:(?:static|inline)\s+)?(?:void|bool|unsigned long)\s+(\w+)\([^;\n]*\)[^\n]*\n\{", masked, re.M):
        if match[1] not in names:
            continue
        found.add(match[1])
        start = masked.index("{", match.start())
        depth = 1
        end = start + 1
        while depth:
            depth += (masked[end] == "{") - (masked[end] == "}")
            end += 1
        line = text.count("\n", 0, match.start()) + 1
        definitions.append(f'#line {line} "{source}"\n{text[match.start():end]}\n')
    if found != names:
        raise RuntimeError(f"Missing production functions: {names - found}")
    return "\n".join(definitions)


def main() -> None:
    cases = [
        "approved", "http200_without_open", "situation_failure", "deferred_approval",
        "reopen_ghost", "reopen_survivor", "is_open_blocked", "tagger",
        "admin_tagger", "admin_ready", "setting", "invalid_tag", "timeout",
        "held_pending", "priority_scheduler", "pending_admin", "pending_admin_rate",
        "failure_held", "timeout_held", "removal_rearms", "miss_does_not_rearm",
        "different_tag", "reset_ready", "reset_setting", "reset_tagger",
        "non_ghost_then_ghost", "reopen_after_removal", "timeout_clock_wrap",
        "normal_poll_resume", "late_approval_identity", "late_failure_identity", "cancelled_late_approval",
        "unknown_scan_preserves_latch", "pending_unavailable_reader",
        "mode_ready_to_activate_device_static", "mode_game_change_keeps_relay_quiet", "mode_ready_ignores_device_rearm",
        "card_upload_setting", "card_upload_ready", "card_upload_activate",
        "card_upload_cancels_approval", "card_upload_clears_failed_user",
        "card_upload_during_approval_poll",
        "card_upload_late_open", "card_upload_late_github",
        "card_upload_maintenance_poll",
        "card_upload_exit_setting", "card_upload_exit_ready", "card_upload_exit_activate",
        "card_upload_exit_defers_gameplay",
        "card_upload_exit_new_open", "card_upload_exit_new_github", "normal_ota_once",
    ]
    with tempfile.TemporaryDirectory(prefix="revival-host-tests-") as directory:
        build = Path(directory)
        write_sensor_types(build)
        names = {"CardChecking", "SolenoidInit", "SolenoidOn", "SolenoidOff", "SolenoidPulse", "NeoBlinkPurple",
                 "RfidLoop", "AdminCardPollReady", "AdminCardPollPending"}
        (build / "sensor_under_test.inc").write_text(extract_functions(DEVICE / "sensor.ino", names))
        (build / "loop_under_test.inc").write_text(extract_functions(DEVICE / "HAS1_revival_machine.ino", {"loop"}))
        constants = {"SOLENOID_PIN", "SOLENOID_PULSE_MS", "SOLENOID_REVIVAL_PULSE_MS",
                     "WIFI_POLL_INTERVAL_DEFAULT_MS", "WIFI_POLL_INTERVAL_ACTIVATE_MS", "RFID_DEBOUNCE_MS",
                     "GHOST_OPEN_TIMEOUT_MS", "REVIVAL_APPROVAL_TIMEOUT_MS", "REVIVAL_APPROVAL_POLL_MS",
                     "REVIVAL_ADMIN_POLL_MS", "RFID_REARM_ABSENT_MS"}
        defines = []
        for line in ((DEVICE / "library_and_pin.h").read_text() + "\n" +
                     (DEVICE / "HAS1_revival_machine.h").read_text()).splitlines():
            match = re.match(r"#define\s+(\w+)\b", line)
            if match and match[1] in constants:
                defines.append(line)
        if len(defines) != len(constants):
            raise RuntimeError("Missing production relay/poll constants")
        (build / "production_constants.inc").write_text("\n".join(defines) + "\n")
        binary = build / "revival_host_tests"
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-I", str(build), "-I", str(DEVICE / "tests" / "fakes"), "-I", str(DEVICE), str(TESTS / "host_tests.cpp"),
                        "-o", str(binary)], check=True)
        for case in cases:
            subprocess.run([str(binary), case], check=True)
    print(f"PASS: {len(cases)} production-path regression cases")


if __name__ == "__main__":
    main()
