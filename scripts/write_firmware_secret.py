#!/usr/bin/env python3
"""Write a firmware HMAC header from the deployment environment without logging it."""

import argparse
import json
import os
from pathlib import Path

LINK_VALIDATION_SECRET = "TAGMACHINE_CI_LINK_VALIDATION_PUBLIC_KEY_NEVER_RELEASE"


def checked_secret(value, allow_link_validation=False):
    placeholders = {
        "CHANGE_THIS_TO_YOUR_SECRET",
        "REPLACE_WITH_DEPLOYMENT_SECRET",
        "__COMPILE_ONLY_DO_NOT_DEPLOY__",
    }
    normalized = value.strip() if value else ""
    if (not normalized or normalized in placeholders or
            (normalized == LINK_VALIDATION_SECRET and not allow_link_validation)):
        raise ValueError("HMAC_SECRET is missing or a placeholder; deployment is disabled")
    if any(ord(character) < 32 for character in value):
        raise ValueError("HMAC_SECRET must not contain control characters")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device_dir", type=Path)
    parser.add_argument(
        "--allow-link-validation-secret",
        action="store_true",
        help="allow the public CI key only for a non-published link/size check",
    )
    args = parser.parse_args()
    try:
        value = checked_secret(
            os.environ.get("HMAC_SECRET"), args.allow_link_validation_secret
        )
    except ValueError as error:
        parser.error(str(error))
    # JSON string quoting handles quotes/backslashes without shell interpolation.
    (args.device_dir / "secrets.h").write_text(
        f"#pragma once\n#define HMAC_SECRET {json.dumps(value, ensure_ascii=True)}\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
