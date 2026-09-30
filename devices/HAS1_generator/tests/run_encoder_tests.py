#!/usr/bin/env python3
"""Run production encoder decoder/lifecycle with host Arduino/FreeRTOS fakes.

This verifies state transitions and pending-count handoff, not ISR latency,
real electrical noise or concurrent task scheduling on the ESP32.
"""
from pathlib import Path
import os
import subprocess
import tempfile


def main() -> None:
    source = Path(__file__).resolve().with_name("encoder_tests.cpp")
    with tempfile.TemporaryDirectory(prefix="has1-generator-encoder-tests-") as directory:
        binary = Path(directory) / "encoder_tests"
        subprocess.run([
            os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
            str(source), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
