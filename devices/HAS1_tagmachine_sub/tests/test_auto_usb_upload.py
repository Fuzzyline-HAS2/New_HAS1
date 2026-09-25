import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from unittest.mock import patch


SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
import auto_usb_upload as uploader  # noqa: E402


COMMIT = "05ad44710616cf9c642c858c1567f56cbee4159f"


def release_fixture(version=4, *, provenance_updates=None):
    image = bytearray(64)
    image[0] = 0xE9
    image[1] = 1
    image[3] = 0x20  # 4 MB flash-size ID in the image header.
    image[12:14] = (5).to_bytes(2, "little")  # ESP32-C3 chip ID.
    update_signature = bytes(range(32))
    provenance = {
        "schema": 1,
        "device": uploader.DEVICE,
        "firmware_version": version,
        "partition_version": uploader.PARTITION_VERSION,
        "partition_scheme": uploader.PARTITION_SCHEME,
        "fqbn": uploader.RELEASE_FQBN,
        "esp32_core": uploader.CORE_VERSION,
        "source_commit": COMMIT,
        "dependencies": {
            "SecureOTA": {
                "commit": uploader.SECUREOTA_REVISION,
                "revision": uploader.SECUREOTA_REVISION,
            }
        },
        "source_sha256": {
            f"devices/{uploader.DEVICE}/{uploader.DEVICE}.ino": "0" * 64
        },
    }
    if provenance_updates:
        provenance.update(provenance_updates)
    downloaded = {
        "update.bin": bytes(image),
        "update.sig": update_signature,
        "ota.txt": (
            f"IGOTA1|{uploader.DEVICE}|{version}|1|default|"
            f"{update_signature.hex()}\n"
        ).encode(),
        "ota.sig": b"s" * 32,
        "version.txt": str(version).encode(),
        "partition_version.txt": b"1",
        "build-provenance.json": (
            json.dumps(provenance, sort_keys=True) + "\n"
        ).encode(),
    }
    tag = uploader.release_tag(version)
    release = {
        "id": 100,
        "tag_name": tag,
        "target_commitish": COMMIT,
        "draft": False,
        "prerelease": False,
        "published_at": "2026-09-25T00:00:00Z",
        "html_url": f"https://github.com/{uploader.REPOSITORY}/releases/tag/{tag}",
        "assets": [],
    }
    for asset_id, name in enumerate(uploader.EXPECTED_ASSETS, 1):
        data = downloaded[name]
        release["assets"].append(
            {
                "id": asset_id,
                "name": name,
                "state": "uploaded",
                "size": len(data),
                "digest": "sha256:" + hashlib.sha256(data).hexdigest(),
                "browser_download_url": uploader.expected_asset_url(tag, name),
            }
        )
    return release, downloaded


def pin_for_release(release):
    return {
        "source_commit": release["target_commitish"],
        "assets": {
            asset["name"]: asset["digest"].removeprefix("sha256:")
            for asset in release["assets"]
        },
    }


class AutoUsbUploadTests(unittest.TestCase):
    def setUp(self):
        self.production_pins = uploader.PINNED_RELEASES
        release, _ = release_fixture()
        uploader.PINNED_RELEASES = {4: pin_for_release(release)}

    def tearDown(self):
        uploader.PINNED_RELEASES = self.production_pins

    def test_exact_usb_fqbn_uses_460800_and_default_partition(self):
        self.assertIn("dfrobot_beetle_esp32c3", uploader.UPLOAD_FQBN)
        self.assertIn("UploadSpeed=460800", uploader.UPLOAD_FQBN)
        self.assertIn("CDCOnBoot=cdc", uploader.UPLOAD_FQBN)
        self.assertIn("PartitionScheme=default", uploader.UPLOAD_FQBN)
        self.assertIn("EraseFlash=none", uploader.UPLOAD_FQBN)

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

    def test_auto_candidates_accept_beetle_native_usb_cdc(self):
        native = uploader.UsbPort(
            "/dev/cu.usbmodem1101",
            "Serial Port (USB)",
            "0x303a",
            "0x1001",
            "10:B4:1D:23:8E:DC",
        )
        self.assertEqual(
            uploader.fresh_candidates([native], set(), set()), [native]
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
            uploader.fresh_candidates(ports, {"/dev/ttyUSB0"}, set()), []
        )
        self.assertEqual(
            uploader.fresh_candidates(ports, set(), {"second"}), [ports[0]]
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

    def test_valid_release_payload_returns_exact_image_digest(self):
        release, downloaded = release_fixture()
        verified = uploader.validate_release_payload(
            release, COMMIT, downloaded, version=4
        )
        self.assertEqual(verified.image, downloaded["update.bin"])
        self.assertEqual(
            verified.image_sha256,
            hashlib.sha256(downloaded["update.bin"]).hexdigest(),
        )
        self.assertEqual(verified.source_commit, COMMIT)

    def test_tampered_image_is_rejected_by_github_digest(self):
        release, downloaded = release_fixture()
        downloaded["update.bin"] += b"tampered"
        with self.assertRaisesRegex(RuntimeError, "크기|SHA-256"):
            uploader.validate_release_payload(release, COMMIT, downloaded, version=4)

    def test_wrong_provenance_is_rejected_after_matching_asset_digest(self):
        release, downloaded = release_fixture(
            provenance_updates={"device": "HAS1_escape_main"}
        )
        uploader.PINNED_RELEASES = {4: pin_for_release(release)}
        with self.assertRaisesRegex(RuntimeError, "provenance"):
            uploader.validate_release_payload(release, COMMIT, downloaded, version=4)

    def test_release_commit_must_match_git_tag_commit(self):
        release, downloaded = release_fixture()
        with self.assertRaisesRegex(RuntimeError, "commit"):
            uploader.validate_release_payload(release, "f" * 40, downloaded, version=4)

    def test_missing_or_extra_release_assets_are_rejected(self):
        release, _ = release_fixture()
        release["assets"].pop()
        with self.assertRaisesRegex(RuntimeError, "asset 구성"):
            uploader.validate_release_metadata(release, version=4)

        release, _ = release_fixture()
        extra = dict(release["assets"][0])
        extra["id"] = 999
        extra["name"] = "unexpected.bin"
        release["assets"].append(extra)
        with self.assertRaisesRegex(RuntimeError, "asset 구성"):
            uploader.validate_release_metadata(release, version=4)

    def test_ota_manifest_must_match_update_signature(self):
        release, downloaded = release_fixture()
        downloaded["ota.txt"] = downloaded["ota.txt"].replace(b"00", b"ff", 1)
        ota_asset = next(
            asset for asset in release["assets"] if asset["name"] == "ota.txt"
        )
        ota_asset["size"] = len(downloaded["ota.txt"])
        ota_asset["digest"] = (
            "sha256:" + hashlib.sha256(downloaded["ota.txt"]).hexdigest()
        )
        uploader.PINNED_RELEASES = {4: pin_for_release(release)}
        with self.assertRaisesRegex(RuntimeError, "canonical"):
            uploader.validate_release_payload(release, COMMIT, downloaded, version=4)

    def test_unpinned_version_or_changed_release_digest_is_rejected(self):
        release, _ = release_fixture()
        uploader.PINNED_RELEASES = {}
        with self.assertRaisesRegex(RuntimeError, "allowlist"):
            uploader.validate_release_metadata(release, version=4)

        uploader.PINNED_RELEASES = {4: pin_for_release(release)}
        update = next(
            asset for asset in release["assets"] if asset["name"] == "update.bin"
        )
        update["digest"] = "sha256:" + "f" * 64
        with self.assertRaisesRegex(RuntimeError, "고정값"):
            uploader.validate_release_metadata(release, version=4)

    def test_upload_command_is_app_only_and_uses_release_file(self):
        command = uploader.upload_command(
            "arduino-cli", "/dev/ttyUSB0", Path("/tmp/board-1/update.bin")
        )
        self.assertEqual(command[1], "upload")
        self.assertIn("UploadSpeed=460800", command[command.index("--fqbn") + 1])
        self.assertEqual(command[command.index("--port") + 1], "/dev/ttyUSB0")
        self.assertEqual(
            command[command.index("--input-file") + 1],
            "/tmp/board-1/update.bin",
        )
        self.assertEqual(command[command.index("--programmer") + 1], "esptool")
        self.assertNotIn("--input-dir", command)
        self.assertIn("--verify", command)

    def test_each_board_gets_a_byte_identical_fresh_image(self):
        release, downloaded = release_fixture()
        verified = uploader.validate_release_payload(
            release, COMMIT, downloaded, version=4
        )
        with tempfile.TemporaryDirectory() as work:
            first = uploader.materialize_image(verified, Path(work) / "board-1")
            second = uploader.materialize_image(verified, Path(work) / "board-2")
            self.assertEqual(first.read_bytes(), verified.image)
            self.assertEqual(second.read_bytes(), verified.image)
            self.assertNotEqual(first.parent, second.parent)

    def test_device_baseline_must_match_default_partition_and_app0(self):
        partition = b"p" * uploader.PARTITION_TABLE_SIZE
        tools = uploader.FlashTools(
            Path("/tmp/esptool"), hashlib.sha256(partition).hexdigest(), b"app0"
        )
        uploader.validate_device_baseline(partition, b"app0", tools)
        with self.assertRaisesRegex(RuntimeError, "파티션"):
            uploader.validate_device_baseline(b"other", b"app0", tools)
        with self.assertRaisesRegex(RuntimeError, "app0"):
            uploader.validate_device_baseline(partition, b"ota1", tools)

    def test_flash_tools_use_build_recipe_csv_not_stale_default_bin(self):
        csv = b"# generated by the build recipe\n"
        stale_bin = b"stale packaged default.bin"
        app0 = b"a" * uploader.OTA_DATA_SIZE
        generated_table_sha256 = "a" * 64

        with tempfile.TemporaryDirectory() as data:
            packages = Path(data) / "packages" / "esp32"
            partitions = (
                packages
                / "hardware"
                / "esp32"
                / uploader.CORE_VERSION
                / "tools"
                / "partitions"
            )
            partitions.mkdir(parents=True)
            (partitions / "default.csv").write_bytes(csv)
            (partitions / "default.bin").write_bytes(stale_bin)
            (partitions / "boot_app0.bin").write_bytes(app0)
            esptool = (
                packages
                / "tools"
                / "esptool_py"
                / uploader.ESPTOOL_VERSION
                / "esptool"
            )
            esptool.parent.mkdir(parents=True)
            esptool.write_bytes(b"")

            with (
                patch.object(uploader, "run_json", return_value=data),
                patch.object(
                    uploader,
                    "DEFAULT_PARTITION_CSV_SHA256",
                    hashlib.sha256(csv).hexdigest(),
                ),
                patch.object(
                    uploader,
                    "DEFAULT_PARTITION_TABLE_SHA256",
                    generated_table_sha256,
                ),
                patch.object(
                    uploader,
                    "BOOT_APP0_SHA256",
                    hashlib.sha256(app0).hexdigest(),
                ),
            ):
                tools = uploader.load_flash_tools("arduino-cli")

        self.assertEqual(tools.partition_table_sha256, generated_table_sha256)
        self.assertEqual(tools.boot_app0, app0)

    def test_baseline_read_command_is_read_only_at_460800(self):
        tools = uploader.FlashTools(Path("/tmp/esptool"), "0" * 64, b"app0")
        command = uploader.read_flash_command(
            tools,
            "/dev/ttyUSB0",
            uploader.OTA_DATA_OFFSET,
            uploader.OTA_DATA_SIZE,
            Path("/tmp/otadata.bin"),
        )
        self.assertIn("read-flash", command)
        self.assertNotIn("write-flash", command)
        self.assertEqual(command[command.index("--baud") + 1], "460800")
        self.assertIn(hex(uploader.OTA_DATA_OFFSET), command)

    def test_cli_requires_version_and_has_no_local_build_or_secret_options(self):
        args = uploader.parse_args(["--expected-version", "4", "--dry-run"])
        self.assertEqual(args.expected_version, 4)
        self.assertFalse(hasattr(args, "secret_file"))
        self.assertFalse(hasattr(args, "libraries_dir"))
        self.assertFalse(hasattr(args, "refresh_dependencies"))
        with redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                uploader.parse_args(["--dry-run"])
            with self.assertRaises(SystemExit):
                uploader.parse_args(["--expected-version", "04", "--dry-run"])

    def test_download_hosts_are_restricted_to_https_github(self):
        self.assertTrue(
            uploader.trusted_download_url(
                "https://release-assets.githubusercontent.com/example"
            )
        )
        self.assertFalse(
            uploader.trusted_download_url("http://github.com/example")
        )
        self.assertFalse(
            uploader.trusted_download_url("https://githubusercontent.com.evil.test/x")
        )

    def test_api_token_is_never_sent_to_release_asset_hosts(self):
        with patch.dict(
            uploader.os.environ, {"GH_TOKEN": "github_pat_test"}, clear=True
        ):
            api_headers = uploader.github_request_headers(api=True)
            asset_headers = uploader.github_request_headers(api=False)

        self.assertEqual(
            api_headers.get("Authorization"), "Bearer github_pat_test"
        )
        self.assertNotIn("Authorization", asset_headers)

    def test_github_token_is_used_when_gh_token_is_absent(self):
        with patch.dict(
            uploader.os.environ, {"GITHUB_TOKEN": "github_token_test"}, clear=True
        ):
            headers = uploader.github_request_headers(api=True)
        self.assertEqual(headers.get("Authorization"), "Bearer github_token_test")

    def test_redirect_drops_authorization_on_host_change(self):
        request = uploader.urllib.request.Request(
            "https://api.github.com/repos/example/release",
            headers={"Authorization": "Bearer secret"},
        )
        redirected = uploader.SafeRedirect().redirect_request(
            request,
            None,
            302,
            "Found",
            {},
            "https://github.com/example/release",
        )
        self.assertIsNotNone(redirected)
        self.assertNotIn("Authorization", redirected.headers)


if __name__ == "__main__":
    unittest.main()
