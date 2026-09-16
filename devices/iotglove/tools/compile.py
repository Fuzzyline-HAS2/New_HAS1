#!/usr/bin/env python3
"""Compile isolated IoT glove copies; never bump versions, upload or publish."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
from firmware_targets import ESP32_CORE_VERSION, TARGETS  # noqa: E402

PROFILES = {
    "ttgo": ("iotglove", 0),
    "training": ("iotglove", 1),
    "beetle": ("iotglove_beetle", 0),
}
SOURCE_EXTENSIONS = {".ino", ".h", ".hpp", ".c", ".cpp", ".S", ".s", ".tpp", ".inc"}


def stage_sketch(source, destination):
    destination.mkdir()
    for entry in source.iterdir():
        if entry.name == "secrets.h":
            continue
        if entry.is_file() and entry.suffix in SOURCE_EXTENSIONS:
            shutil.copy2(entry, destination / entry.name)
        elif entry.is_dir() and entry.name == "src":
            shutil.copytree(entry, destination / "src")
    (destination / "secrets.h").write_text(
        '#pragma once\n#define HMAC_SECRET "__COMPILE_ONLY_DO_NOT_DEPLOY__"\n',
        encoding="utf-8",
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", nargs="?", choices=["all", *PROFILES], default="all")
    parser.add_argument("--arduino-cli", default=os.environ.get("ARDUINO_CLI", "arduino-cli"))
    parser.add_argument("--config-file", type=Path)
    parser.add_argument("--libraries-dir", type=Path, default=os.environ.get("IOTGLOVE_LIBRARY_DIR"))
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build" / "iotglove-compile-only")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if not args.libraries_dir:
        parser.error("--libraries-dir (or IOTGLOVE_LIBRARY_DIR) must name prepared dependencies")
    libraries = Path(args.libraries_dir).resolve()
    manifest_path = libraries / "iotglove-dependencies.json"
    if not manifest_path.is_file():
        parser.error("Run tools/prepare_libraries.py into --libraries-dir first")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    patch_hash = hashlib.sha256(Path(__file__).with_name("has2-wifi-result-api.patch").read_bytes()).hexdigest()
    if (manifest.get("HAS2_Wifi", {}).get("branch") != "first_store" or
            manifest.get("HAS2_Wifi", {}).get("patch_sha256") != patch_hash):
        parser.error("Prepared HAS2_Wifi is not first_store with the current patch; prepare a fresh directory")
    cli = [args.arduino_cli]
    if args.config_file:
        cli += ["--config-file", str(args.config_file.resolve())]
    platforms = json.loads(subprocess.check_output(cli + ["core", "list", "--format", "json"], text=True))
    installed = {item["id"]: item.get("installed_version") for item in platforms.get("platforms", [])}
    if installed.get("esp32:esp32") != ESP32_CORE_VERSION:
        parser.error(f"Install esp32:esp32@{ESP32_CORE_VERSION}; found {installed.get('esp32:esp32')}")
    profiles = PROFILES if args.profile == "all" else [args.profile]
    for profile in profiles:
        target, training = PROFILES[profile]
        directory, fqbn = TARGETS[target]
        output = args.output_dir.resolve() / profile
        output.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=f"iotglove-{profile}-") as work:
            sketch = Path(work) / Path(directory).name
            stage_sketch(ROOT / directory, sketch)
            command = cli + [
                "compile", "--fqbn", fqbn, "--jobs", str(args.jobs),
                "--build-path", str(Path(work) / "build"),
                "--output-dir", str(output),
                "--libraries", str(libraries),
                "--library", str(ROOT / "libraries" / "IoTGloveProtocol"),
                "--build-property", f"compiler.cpp.extra_flags=-DIOTGLOVE_TRAINING={training} -DIOTGLOVE_COMPILE_ONLY=1",
                str(sketch),
            ]
            print(f"Compile {profile}: {fqbn}", flush=True)
            subprocess.run(command, check=True)
        (output / "COMPILE_ONLY.txt").write_text(
            "Validation build with a placeholder HMAC key. Do not flash or publish.\n",
            encoding="utf-8",
        )
        shutil.copy2(manifest_path, output / manifest_path.name)


if __name__ == "__main__":
    main()
