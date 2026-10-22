#!/usr/bin/env python3
"""Build the actual transport/parser and deterministic fault/real-socket tests."""
from pathlib import Path
import os
import subprocess
import tempfile
lib = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='has2-bounded-transport-') as directory:
    exe = Path(directory) / 'test'
    sanitizers = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.environ.get('SANITIZE') == '1' else []
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-DHAS2_BT_HOST', '-pthread', '-I', str(lib),
                    str(lib / 'has2_bounded_transport.cpp'),
                    str(lib / 'tests/bounded_transport_test.cpp'), '-o', str(exe)] + sanitizers, check=True, timeout=60)
    subprocess.run([str(exe)], check=True, timeout=30)
