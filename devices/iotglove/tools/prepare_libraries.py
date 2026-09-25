#!/usr/bin/env python3
"""Prepare reproducible firmware dependencies in an isolated library directory."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

PATCH = Path(__file__).with_name("has2-wifi-result-api.patch")
SECUREOTA_REVISION = "162db758e6806895ef10ca39b101f9f8753bdc20"
LIBRARIES = {
    "SecureOTA": (
        "https://github.com/Fuzzyline-HAS2/SecureOTA", None, ".", SECUREOTA_REVISION
    ),
    "HAS2_Wifi": (
        "https://github.com/Fuzzyline-HAS/libraries", "first_store", "HAS2_Wifi", None
    ),
    "SimpleTimer": (
        "https://github.com/marcelloromani/Arduino-SimpleTimer", None, "SimpleTimer", None
    ),
}


def directory_sha256(path):
    """Hash a copied library tree independently of its absolute location."""
    digest = hashlib.sha256()
    for item in sorted(
        (entry for entry in path.rglob("*") if entry.is_file()),
        key=lambda entry: entry.relative_to(path).as_posix(),
    ):
        relative = item.relative_to(path).as_posix().encode("utf-8")
        digest.update(len(relative).to_bytes(4, "big"))
        digest.update(relative)
        digest.update(hashlib.sha256(item.read_bytes()).digest())
    return digest.hexdigest()


def clone_checkout(url, checkout, branch=None, revision=None):
    """Clone one branch tip or fetch one exact commit into a detached checkout."""
    if revision:
        checkout.mkdir()
        subprocess.run(["git", "init", "--quiet", str(checkout)], check=True)
        subprocess.run(
            ["git", "-C", str(checkout), "remote", "add", "origin", url], check=True
        )
        subprocess.run(
            [
                "git", "-C", str(checkout), "fetch", "--quiet", "--depth", "1",
                "--no-tags", "origin", revision,
            ],
            check=True,
        )
        subprocess.run(
            ["git", "-C", str(checkout), "checkout", "--quiet", "--detach", "FETCH_HEAD"],
            check=True,
        )
        return

    command = ["git", "clone", "--depth", "1"]
    if branch:
        command += ["--branch", branch]
    subprocess.run(command + [url, str(checkout)], check=True)


def prepare(destination):
    if not PATCH.is_file():
        raise SystemExit(f"Missing required HAS2_Wifi patch: {PATCH}")
    destination.mkdir(parents=True, exist_ok=True)
    for name in LIBRARIES:
        if (destination / name).exists():
            raise SystemExit(f"Refusing to overwrite {destination / name}; use a fresh directory")
    provenance = {}
    with tempfile.TemporaryDirectory(prefix="iotglove-dependencies-") as work:
        for name, (url, branch, subdirectory, revision) in LIBRARIES.items():
            checkout = Path(work) / name
            clone_checkout(url, checkout, branch=branch, revision=revision)
            commit = subprocess.check_output(
                ["git", "-C", str(checkout), "rev-parse", "HEAD"], text=True
            ).strip()
            if revision and commit != revision:
                raise SystemExit(
                    f"Dependency revision mismatch for {name}: expected {revision}, got {commit}"
                )
            provenance[name] = {"url": url, "branch": branch, "commit": commit}
            if revision:
                provenance[name]["revision"] = revision
            if name == "SecureOTA" and not (checkout / "scripts" / "ci_deploy.py").is_file():
                raise SystemExit(
                    f"Pinned SecureOTA {commit} is missing required scripts/ci_deploy.py"
                )
            if name == "HAS2_Wifi":
                # Fail on upstream drift instead of silently compiling without checked APIs.
                subprocess.run(["git", "-C", str(checkout), "apply", "--check", str(PATCH)], check=True)
                subprocess.run(["git", "-C", str(checkout), "apply", str(PATCH)], check=True)
                provenance[name]["patch_sha256"] = hashlib.sha256(PATCH.read_bytes()).hexdigest()
        # Copy only after all downloads and patch checks pass.
        for name, (_, _, subdirectory, _) in LIBRARIES.items():
            shutil.copytree(
                Path(work) / name / subdirectory, destination / name,
                ignore=shutil.ignore_patterns(".git"),
            )
            provenance[name]["tree_sha256"] = directory_sha256(destination / name)
    (destination / "iotglove-dependencies.json").write_text(
        json.dumps(provenance, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(provenance, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--libraries-dir", required=True, type=Path)
    prepare(parser.parse_args().libraries_dir.resolve())
