#!/usr/bin/env python3
"""Test the portable C++11 NDEF encoder with exact fixtures and ASan/UBSan."""
from pathlib import Path
import os
import subprocess
import tempfile

TESTS = Path(__file__).resolve().parent
DEVICE = TESTS.parent


def main():
    with tempfile.TemporaryDirectory(prefix="revival-card-ndef-tests-") as directory:
        binary = Path(directory) / "card_ndef_tests"
        subprocess.run([
            os.environ.get("CXX", "c++"), "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(DEVICE),
            str(DEVICE / "card_ndef.cpp"), str(TESTS / "card_ndef_tests.cpp"),
            "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
