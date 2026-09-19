#!/usr/bin/env python3
"""Build and execute the real portable BLE state machine with sanitizer checks."""

from pathlib import Path
import os
import subprocess
import tempfile

TESTS = Path(__file__).resolve().parent
SOURCE = TESTS.parent / "src"


def main():
    with tempfile.TemporaryDirectory(prefix="has1-ble-tests-") as temporary:
        executable = Path(temporary) / "beacon-tests"
        compiler = [
            "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-O1", "-g", "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
        ]
        subprocess.run(
            compiler + [
                "-I", str(SOURCE), str(SOURCE / "BeaconState.cpp"),
                str(TESTS / "state_machine_test.cpp"), "-o", str(executable),
            ],
            check=True,
        )
        environment = os.environ.copy()
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        subprocess.run([str(executable)], check=True, env=environment, timeout=30)
        adapter = Path(temporary) / "adapter-tests"
        subprocess.run(
            compiler + [
                "-I", str(TESTS / "stubs"), "-I", str(SOURCE),
                str(SOURCE / "BeaconState.cpp"), str(SOURCE / "HAS1BleBeacon.cpp"),
                str(TESTS / "adapter_test.cpp"), "-o", str(adapter),
            ],
            check=True,
        )
        # The real adapter is a boot-lifetime singleton; give each case a fresh
        # process rather than exposing production-only reset/testing hooks.
        for scenario in (
            "startup_failure", "registration_failure", "immediate_callback_idempotence",
            "successful_status_not_final", "timely_mailbox_consumption",
            "wrong_duplicate_events", "unavailable_transport_service",
        ):
            subprocess.run([str(adapter), scenario], check=True, env=environment, timeout=30)


if __name__ == "__main__":
    main()
