#!/usr/bin/env python3
"""Isolated generator compile validation; public OTA test key, never deploy."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location("glove_build", ROOT / "devices/iotglove/tools/compile.py")
_build = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_build)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arduino-cli", default=os.environ.get("ARDUINO_CLI", "arduino-cli"))
    parser.add_argument("--config-file", type=Path)
    parser.add_argument("--libraries-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/blackbox-generator")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--blackbox-mode", choices=("off", "flash", "mqtt"), default="flash")
    args = parser.parse_args()
    cli = [args.arduino_cli]
    if args.config_file:
        cli += ["--config-file", str(args.config_file.resolve())]
    installed = json.loads(subprocess.check_output(cli + ["core", "list", "--format", "json"], text=True))
    versions = {p["id"]: p.get("installed_version") for p in installed.get("platforms", [])}
    if versions.get("esp32:esp32") != _build.ESP32_CORE_VERSION:
        parser.error("Install esp32:esp32@" + _build.ESP32_CORE_VERSION)
    directory, fqbn = _build.TARGETS["HAS1_generator"]
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="blackbox-generator-") as work:
        sketch = Path(work) / "HAS1_generator"
        _build.stage_sketch(ROOT / directory, sketch, check_ota_paths=True)
        command = cli + ["compile", "--fqbn", fqbn, "--jobs", str(args.jobs),
                         "--build-path", str(Path(work) / "build"), "--output-dir", str(output),
                         "--libraries", str(args.libraries_dir.resolve())]
        # These are the generator's existing repository-owned dependencies,
        # plus the new blackbox. Do not substitute the glove's HAS2_Wifi patch.
        for name in ("HAS2_Wifi", "HAS1BleBeacon", "DFRobotDFPlayerMini", "HAS1BlackBox"):
            command += ["--library", str(ROOT / "libraries" / name)]
        flags = _build.blackbox_flags("ttgo", args.blackbox_mode).replace("IOTGLOVE_", "HAS1_GENERATOR_")
        if flags:
            command += ["--build-property", "compiler.cpp.extra_flags=" + flags.strip()]
        subprocess.run(command + [str(sketch)], check=True)
    (output / "COMPILE_ONLY.txt").write_text(
        "Public non-production OTA key. Do not flash or publish.\n", encoding="utf-8")


if __name__ == "__main__":
    main()
