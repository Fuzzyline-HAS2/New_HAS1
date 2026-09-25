#!/usr/bin/env python3
"""Build and run host-side tests for TagMachine Beetle policies."""

from pathlib import Path
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="tagmachine-sub-tests-") as tmp:
        tests = sorted(HERE.glob("*_tests.cpp"))
        if not tests:
            raise SystemExit("No TagMachine Beetle host tests found")
        for source in tests:
            binary = Path(tmp) / source.stem
            subprocess.run(
                [
                    "c++",
                    "-std=c++11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)
    subprocess.run(
        [
            sys.executable,
            "-m",
            "unittest",
            "discover",
            "-s",
            str(HERE),
            "-p",
            "test_*.py",
        ],
        check=True,
    )
    print(f"tagmachine_sub host tests ({len(tests)} C++ executables + Python): PASS")


if __name__ == "__main__":
    main()
