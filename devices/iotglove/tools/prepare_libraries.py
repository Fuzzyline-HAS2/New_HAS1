#!/usr/bin/env python3
"""Prepare current first_store dependencies in a new, isolated library directory."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

PATCH = Path(__file__).with_name("has2-wifi-result-api.patch")
LIBRARIES = {
    "SecureOTA": ("https://github.com/Fuzzyline-HAS2/SecureOTA", None, "."),
    "HAS2_Wifi": ("https://github.com/Fuzzyline-HAS/libraries", "first_store", "HAS2_Wifi"),
    "SimpleTimer": ("https://github.com/marcelloromani/Arduino-SimpleTimer", None, "SimpleTimer"),
}


def prepare(destination):
    if not PATCH.is_file():
        raise SystemExit(f"Missing required HAS2_Wifi patch: {PATCH}")
    destination.mkdir(parents=True, exist_ok=True)
    for name in LIBRARIES:
        if (destination / name).exists():
            raise SystemExit(f"Refusing to overwrite {destination / name}; use a fresh directory")
    provenance = {}
    with tempfile.TemporaryDirectory(prefix="iotglove-dependencies-") as work:
        for name, (url, branch, subdirectory) in LIBRARIES.items():
            checkout = Path(work) / name
            command = ["git", "clone", "--depth", "1"]
            if branch:
                command += ["--branch", branch]
            subprocess.run(command + [url, str(checkout)], check=True)
            commit = subprocess.check_output(
                ["git", "-C", str(checkout), "rev-parse", "HEAD"], text=True
            ).strip()
            provenance[name] = {"url": url, "branch": branch, "commit": commit}
            if name == "HAS2_Wifi":
                # Fail on upstream drift instead of silently compiling without checked APIs.
                subprocess.run(["git", "-C", str(checkout), "apply", "--check", str(PATCH)], check=True)
                subprocess.run(["git", "-C", str(checkout), "apply", str(PATCH)], check=True)
                provenance[name]["patch_sha256"] = hashlib.sha256(PATCH.read_bytes()).hexdigest()
        # Copy only after all downloads and patch checks pass.
        for name, (_, _, subdirectory) in LIBRARIES.items():
            shutil.copytree(
                Path(work) / name / subdirectory, destination / name,
                ignore=shutil.ignore_patterns(".git"),
            )
    (destination / "iotglove-dependencies.json").write_text(
        json.dumps(provenance, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(provenance, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--libraries-dir", required=True, type=Path)
    prepare(parser.parse_args().libraries_dir.resolve())
