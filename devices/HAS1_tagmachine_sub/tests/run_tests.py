#!/usr/bin/env python3
"""Build and run host-side tests for the Beetle recovery policy."""

from pathlib import Path
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="tagmachine-sub-tests-") as tmp:
        binary = Path(tmp) / "recovery_policy_tests"
        subprocess.run(
            [
                "c++",
                "-std=c++11",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(HERE / "recovery_policy_tests.cpp"),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)
    print("tagmachine_sub recovery policy tests: PASS")


if __name__ == "__main__":
    main()
