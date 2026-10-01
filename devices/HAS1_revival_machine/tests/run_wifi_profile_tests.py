#!/usr/bin/env python3
"""Exercise production connection/profile code with a driver lifecycle fake."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

TESTS = Path(__file__).resolve().parent
ROOT = TESTS.parents[2]
LIBRARY = ROOT / "libraries" / "HAS2_Wifi"


def main():
    source = (LIBRARY / "HAS2_Wifi.cpp").read_text()
    masked = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"',
                    lambda m: re.sub(r"[^\n]", " ", m[0]), source, flags=re.S)
    names = {"EnableLegacy1Mbps", "ApplyLegacy1Mbps", "TryConnect"}
    definitions = []
    for match in re.finditer(r"^(?:bool|void) HAS2_Wifi::(\w+)\([^\n]*\)\n\{", masked, re.M):
        if match[1] not in names:
            continue
        names.remove(match[1])
        end = masked.index("{", match.start()) + 1
        depth = 1
        while depth:
            depth += (masked[end] == "{") - (masked[end] == "}")
            end += 1
        definitions.append(source[match.start():end])
    assert not names, f"Missing production methods: {names}"
    # Use the real opt-in default, so changing the production default fails tests.
    header = (LIBRARY / "HAS2_Wifi.h").read_text()
    default = re.search(r"bool legacy1MbpsEnabled\s*=\s*(?:true|false);", header)
    assert default
    sketch = (TESTS.parent / "HAS1_revival_machine.ino").read_text()
    assert sketch.index("has2wifi.EnableLegacy1Mbps();") < sketch.index('has2wifi.Setup("badland");')
    with tempfile.TemporaryDirectory(prefix="revival-wifi-tests-") as directory:
        build = Path(directory)
        (build / "wifi_profile_under_test.inc").write_text("\n".join(definitions))
        (build / "wifi_profile_default.inc").write_text(default[0])
        binary = build / "wifi_profile_tests"
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-I", str(build), str(TESTS / "wifi_profile_tests.cpp"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
