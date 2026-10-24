#!/usr/bin/env python3
"""Pure C++ tests, with sanitizers on supported Unix hosts. No Arduino dependency."""
import os
import pathlib
import subprocess
import tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="blackbox-tests-") as temp:
    executable = pathlib.Path(temp) / "core_test"
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-g"]
    if os.name != "nt":
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    subprocess.run([os.environ.get("CXX", "c++"), *flags, "-I", str(ROOT / "src"), str(ROOT / "tests/core_test.cpp"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
