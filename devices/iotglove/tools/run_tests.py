#!/usr/bin/env python3
"""Build and run the firmware's pure C++ tests without Arduino or hardware."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
GLOVE = ROOT / "devices" / "iotglove"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    tests = sorted((GLOVE / "tests").glob("*_test.cpp"))
    if not tests:
        parser.error("No firmware test sources found")
    with tempfile.TemporaryDirectory(prefix="iotglove-host-tests-") as work:
        for source in tests:
            binary = Path(work) / source.stem
            command = [
                args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-I", str(ROOT / "libraries" / "IoTGloveProtocol" / "src"),
                "-I", str(GLOVE), str(source),
                str(GLOVE / "game_state.cpp"), "-o", str(binary),
            ]
            print(f"Host test: {source.name}", flush=True)
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)
    print(f"Passed {len(tests)} firmware host test executables")


if __name__ == "__main__":
    main()
