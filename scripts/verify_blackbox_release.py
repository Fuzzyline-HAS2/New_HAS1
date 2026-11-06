#!/usr/bin/env python3
"""Reject an official blackbox release whose compiled image lacks MQTT settings.

This checks the image that will be signed and published, not just its source.
Credentials and credential digests are never included in output.
"""

import argparse
import json
import os
from pathlib import Path

from firmware_targets import TARGETS
from write_blackbox_release_config import PREFIXES, configuration


def verify_firmware(device, public, image, broker=None, password=None):
    if device not in TARGETS:
        raise ValueError("Unsupported firmware target")
    if not isinstance(public, dict) or public.get("device") != device or public.get("schema") != 1:
        raise ValueError("Blackbox metadata does not match this firmware target")
    if device not in PREFIXES:
        if public.get("mode") != "not_applicable":
            raise ValueError("Unexpected blackbox mode for this firmware target")
        return False
    # Resolve and validate independently of the generated metadata. Do not print
    # the defines: they contain the current deployment credential.
    _, expected = configuration(device, "mqtt", broker, password, release=True)
    if public != expected:
        raise ValueError("Official blackbox metadata must describe the configured MQTT deployment")
    if not image or image[0] != 0xE9:
        raise ValueError("Expected an ESP32 application image")
    for marker in (b"bb-writer\0", b"bb-upload\0", b"device.boot\0"):
        if marker not in image:
            raise ValueError("Compiled image is missing required blackbox code")
    if broker.encode("utf-8") + b"\0" not in image:
        raise ValueError("Compiled image is missing the configured MQTT broker")
    if password.encode("utf-8") + b"\0" not in image:
        raise ValueError("Compiled image is missing the configured MQTT credential")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, choices=TARGETS)
    parser.add_argument("--public-config", type=Path, required=True)
    parser.add_argument("--firmware", type=Path, required=True)
    args = parser.parse_args()
    try:
        verified = verify_firmware(
            args.device, json.loads(args.public_config.read_text(encoding="utf-8")),
            args.firmware.read_bytes(), os.environ.get("BLACKBOX_BROKER_URI"),
            os.environ.get("BLACKBOX_MQTT_PASSWORD"))
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print("MQTT blackbox image verified" if verified else "Blackbox verification not applicable to this target")


if __name__ == "__main__":
    main()
