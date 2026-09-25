import json
from pathlib import Path
import sys
import tempfile
import unittest


SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
import auto_usb_upload as uploader  # noqa: E402


class AutoUsbUploadTests(unittest.TestCase):
    def test_exact_usb_fqbn_uses_460800_and_ota_partition(self):
        self.assertIn("dfrobot_beetle_esp32c3", uploader.FQBN)
        self.assertIn("UploadSpeed=460800", uploader.FQBN)
        self.assertIn("CDCOnBoot=cdc", uploader.FQBN)
        self.assertIn("PartitionScheme=default", uploader.FQBN)
        self.assertIn("EraseFlash=none", uploader.FQBN)

    def test_modern_board_list_keeps_only_physical_usb_serial(self):
        payload = {
            "detected_ports": [
                {
                    "port": {
                        "address": "/dev/cu.usbserial-123",
                        "label": "Serial Port (USB)",
                        "protocol": "serial",
                        "properties": {
                            "vid": "0x1A86",
                            "pid": "0x55D4",
                            "serialNumber": "ABC123",
                        },
                    }
                },
                {
                    "port": {
                        "address": "/dev/cu.Bluetooth-Incoming-Port",
                        "protocol": "serial",
                        "properties": {},
                    }
                },
                {
                    "port": {
                        "address": "/dev/cu.debug-console",
                        "protocol": "serial",
                        "properties": {"vid": "0x0001", "pid": "0x0002"},
                    }
                },
            ]
        }
        self.assertEqual(
            uploader.parse_usb_ports(payload),
            [
                uploader.UsbPort(
                    "/dev/cu.usbserial-123",
                    "Serial Port (USB)",
                    "0x1a86",
                    "0x55d4",
                    "ABC123",
                )
            ],
        )

    def test_auto_candidates_only_accept_confirmed_beetle_usb_bridge(self):
        beetle = uploader.UsbPort(
            "/dev/ttyUSB0", "WCH", "0x1a86", "0x55d4", "beetle-1"
        )
        other = uploader.UsbPort(
            "/dev/ttyUSB1", "CP210x", "0x10c4", "0xea60", "other"
        )
        self.assertEqual(
            uploader.fresh_candidates([other, beetle], set(), set()), [beetle]
        )

    def test_legacy_board_list_and_existing_port_suppression(self):
        payload = [
            {
                "address": "/dev/ttyUSB0",
                "protocol": "serial",
                "serial_number": "first",
                "boards": [{"name": "Beetle", "vid": "1a86", "pid": "55d4"}],
            },
            {
                "address": "/dev/ttyUSB1",
                "protocol": "serial",
                "serial_number": "second",
                "boards": [{"name": "Other", "vid": "10c4", "pid": "ea60"}],
            },
        ]
        ports = uploader.parse_usb_ports(payload)
        self.assertEqual(len(ports), 2)
        self.assertEqual(
            uploader.fresh_candidates(ports, {"/dev/ttyUSB0"}, set()),
            [],
        )
        self.assertEqual(
            uploader.fresh_candidates(ports, set(), {"second"}),
            [ports[0]],
        )

    def test_multi_board_mode_requires_stable_usb_serial(self):
        with self.assertRaises(RuntimeError):
            uploader.require_stable_sequence_identity(
                uploader.UsbPort("COM3", "WCH", "0x1a86", "0x55d4"), 2
            )
        uploader.require_stable_sequence_identity(
            uploader.UsbPort(
                "COM3", "WCH", "0x1a86", "0x55d4", "stable-serial"
            ),
            2,
        )

    def test_secret_must_be_real_and_is_never_returned_separately(self):
        with tempfile.TemporaryDirectory() as work:
            path = Path(work) / "secrets.h"
            path.write_text('#pragma once\n#define HMAC_SECRET "field-key-123"\n')
            self.assertEqual(uploader.validated_secret_header(path), path.read_text())
            for value in (
                "",
                "CHANGE_THIS_TO_YOUR_SECRET",
                "__COMPILE_ONLY_DO_NOT_DEPLOY__",
                "TAGMACHINE_CI_LINK_VALIDATION_PUBLIC_KEY_NEVER_RELEASE",
            ):
                path.write_text(
                    "#pragma once\n#define HMAC_SECRET " + json.dumps(value) + "\n"
                )
                with self.subTest(value=value), self.assertRaises(RuntimeError):
                    uploader.validated_secret_header(path)

    def test_upload_command_uses_full_image_directory_and_verify(self):
        command = uploader.upload_command(
            "arduino-cli", None, "/dev/ttyUSB0", Path("/tmp/build")
        )
        self.assertEqual(command[1], "upload")
        self.assertIn("UploadSpeed=460800", command[command.index("--fqbn") + 1])
        self.assertEqual(command[command.index("--port") + 1], "/dev/ttyUSB0")
        self.assertEqual(command[command.index("--input-dir") + 1], "/tmp/build")
        self.assertEqual(
            command[command.index("--upload-property") + 1],
            "upload.extra_flags=--no-fast-flash",
        )
        self.assertIn("--verify", command)

    def test_dependency_provenance_detects_modified_cached_library(self):
        with tempfile.TemporaryDirectory() as work:
            libraries = Path(work)
            for name in uploader.REQUIRED_LIBRARY_DIRS:
                (libraries / name).mkdir()
                (libraries / name / "marker.txt").write_text(name)
            for name, version in uploader.REGISTRY_LIBRARY_VERSIONS.items():
                (libraries / name / "library.properties").write_text(
                    f"name={name}\nversion={version}\n"
                )
            patch_hash = uploader.hashlib.sha256(
                uploader.PREPARE_LIBRARIES.with_name(
                    "has2-wifi-result-api.patch"
                ).read_bytes()
            ).hexdigest()
            provenance = {
                "SecureOTA": {
                    "commit": uploader.SECUREOTA_REVISION,
                    "revision": uploader.SECUREOTA_REVISION,
                },
                "HAS2_Wifi": {
                    "branch": "first_store",
                    "patch_sha256": patch_hash,
                },
                "SimpleTimer": {},
            }
            for name in uploader.CUSTOM_LIBRARY_DIRS:
                provenance[name]["tree_sha256"] = uploader.directory_sha256(
                    libraries / name
                )
            (libraries / "iotglove-dependencies.json").write_text(
                json.dumps(provenance)
            )
            self.assertEqual(uploader.validate_library_collection(libraries), libraries)
            (libraries / "SecureOTA" / "marker.txt").write_text("tampered")
            with self.assertRaises(RuntimeError):
                uploader.validate_library_collection(libraries)


if __name__ == "__main__":
    unittest.main()
