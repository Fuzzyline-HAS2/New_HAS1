#!/usr/bin/env python3
"""Write a firmware HMAC header from the deployment environment without logging it."""

import argparse
import json
import os
from pathlib import Path


def checked_secret(value):
    placeholders = {"CHANGE_THIS_TO_YOUR_SECRET", "REPLACE_WITH_DEPLOYMENT_SECRET", "__COMPILE_ONLY_DO_NOT_DEPLOY__"}
    if not value or not value.strip() or value.strip() in placeholders:
        raise ValueError("HMAC_SECRET is missing or a placeholder; deployment is disabled")
    if any(ord(character) < 32 for character in value):
        raise ValueError("HMAC_SECRET must not contain control characters")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device_dir", type=Path)
    args = parser.parse_args()
    try:
        value = checked_secret(os.environ.get("HMAC_SECRET"))
    except ValueError as error:
        parser.error(str(error))
    # JSON string quoting handles quotes/backslashes without shell interpolation.
    (args.device_dir / "secrets.h").write_text(
        f"#pragma once\n#define HMAC_SECRET {json.dumps(value, ensure_ascii=True)}\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
