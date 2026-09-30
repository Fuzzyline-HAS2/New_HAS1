#!/usr/bin/env python3
"""Validate the generator-only recovery override; never modify source versions."""
from pathlib import Path
import re
import sys


def republish_outputs(device: str, partition: str, version: str, root: Path) -> str:
    # Empty input leaves every target on the existing version-bump path.
    if version == "":
        return ""
    if device != "HAS1_generator":
        raise ValueError("republish_version is only supported for HAS1_generator")
    if partition != "false":
        raise ValueError("generator recovery requires update_partition=false")
    if not re.fullmatch(r"[1-9][0-9]*", version):
        raise ValueError("republish_version must be a positive decimal version without leading zeros")
    source = root / "devices" / "HAS1_generator" / "HAS1_generator.ino"
    versions = re.findall(r"^\s*#define\s+FIRMWARE_VER\s+([0-9]+)\s*(?://[^\n]*)?$", source.read_text(), re.MULTILINE)
    if versions != [version]:
        raise ValueError("republish_version must exactly match the source FIRMWARE_VER")
    return f"NEW_VER={version}\nNEW_PARTITION_VER=\n"


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit("usage: generator_republish.py DEVICE UPDATE_PARTITION VERSION")
    try:
        outputs = republish_outputs(*sys.argv[1:], Path(__file__).resolve().parents[1])
    except (ValueError, OSError) as error:
        raise SystemExit(str(error)) from error
    print(outputs, end="")


if __name__ == "__main__":
    main()
