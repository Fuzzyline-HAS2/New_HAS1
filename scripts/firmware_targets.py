#!/usr/bin/env python3
"""Single target/path/FQBN mapping for local builds and manual deployment."""

import argparse
from pathlib import PurePosixPath

ESP32_CORE_VERSION = "3.3.11"


def ttgo_fqbn(speed, partition):
    return (
        f"esp32:esp32:ttgo-t1:UploadSpeed={speed},CPUFreq=240,FlashFreq=80,"
        f"FlashMode=qio,FlashSize=4M,PartitionScheme={partition},"
        "DebugLevel=none,EraseFlash=none"
    )


TARGETS = {
    name: (f"devices/{name}", ttgo_fqbn(115200, "min_spiffs"))
    for name in ("HAS1_itembox", "HAS1_generator", "HAS1_altar", "HAS1_duct")
}
TARGETS.update({
    name: (f"devices/{name}", ttgo_fqbn(921600, "default"))
    for name in ("HAS1_revival_machine", "HAS1_escape_main", "HAS1_tagmachine_main")
})
TARGETS.update({
    "iotglove": ("devices/iotglove", ttgo_fqbn(115200, "min_spiffs")),
    "iotglove_beetle": (
        "devices/iotglove/iotglove_beetle",
        "esp32:esp32:esp32c3:UploadSpeed=115200,CDCOnBoot=cdc,CPUFreq=160,"
        "FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=min_spiffs,"
        "DebugLevel=none,EraseFlash=none",
    ),
})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=TARGETS)
    args = parser.parse_args()
    directory, fqbn = TARGETS[args.target]
    print(f"DEVICE_DIR={directory}")
    print(f"FQBN={fqbn}")
    print(f"RELEASE_TAG={PurePosixPath(directory).name}")


if __name__ == "__main__":
    main()
