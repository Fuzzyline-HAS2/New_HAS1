#!/usr/bin/env python3
"""Compile production bounded transport/parser against deterministic socket faults."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parent
class BoundedHttpTests(unittest.TestCase):
    def test_production_parser_and_socket_deadlines(self):
        with tempfile.TemporaryDirectory(prefix="bounded-http-test-") as tmp:
            binary = Path(tmp) / "http-test"
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "tests/http_stubs"), "-I", str(ROOT / "bounded_http"),
                            str(ROOT / "bounded_http/HAS2BoundedHTTP.cpp"),
                            str(ROOT / "tests/bounded_http_test.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
if __name__ == "__main__": unittest.main()
