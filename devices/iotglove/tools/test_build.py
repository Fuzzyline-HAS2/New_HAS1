"""Regression checks for build isolation and the existing release target contract."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
from firmware_targets import TARGETS  # noqa: E402
from write_firmware_secret import LINK_VALIDATION_SECRET, checked_secret  # noqa: E402

spec = importlib.util.spec_from_file_location("glove_compile", Path(__file__).with_name("compile.py"))
glove_compile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(glove_compile)

prepare_spec = importlib.util.spec_from_file_location(
    "prepare_libraries", Path(__file__).with_name("prepare_libraries.py")
)
prepare_libraries = importlib.util.module_from_spec(prepare_spec)
prepare_spec.loader.exec_module(prepare_libraries)


class BuildContractTests(unittest.TestCase):
    def test_release_workflow_rejects_non_default_branches_before_checkout(self):
        workflow = (ROOT / ".github" / "workflows" / "deploy-firmware.yml").read_text()
        guard = workflow.index("github.event.repository.default_branch")
        checkout = workflow.index("uses: actions/checkout@v4")
        self.assertLess(guard, checkout)
        self.assertIn('if [ "$GITHUB_REF_NAME" != "$DEFAULT_BRANCH" ]', workflow)

    def test_secureota_release_dependency_is_pinned_and_exact_checkout_works(self):
        self.assertEqual(
            prepare_libraries.SECUREOTA_REVISION,
            "162db758e6806895ef10ca39b101f9f8753bdc20",
        )
        with tempfile.TemporaryDirectory() as work:
            source = Path(work) / "source"
            source.mkdir()
            subprocess.run(["git", "init", "--quiet", str(source)], check=True)
            subprocess.run(["git", "-C", str(source), "config", "user.name", "Test"], check=True)
            subprocess.run(
                ["git", "-C", str(source), "config", "user.email", "test@example.invalid"],
                check=True,
            )
            marker = source / "marker"
            marker.write_text("pinned\n")
            subprocess.run(["git", "-C", str(source), "add", "marker"], check=True)
            subprocess.run(["git", "-C", str(source), "commit", "--quiet", "-m", "pinned"], check=True)
            pinned = subprocess.check_output(
                ["git", "-C", str(source), "rev-parse", "HEAD"], text=True
            ).strip()
            marker.write_text("newer\n")
            subprocess.run(["git", "-C", str(source), "commit", "--quiet", "-am", "newer"], check=True)

            checkout = Path(work) / "checkout"
            prepare_libraries.clone_checkout(source.as_uri(), checkout, revision=pinned)
            actual = subprocess.check_output(
                ["git", "-C", str(checkout), "rev-parse", "HEAD"], text=True
            ).strip()
            self.assertEqual(actual, pinned)
            self.assertEqual((checkout / "marker").read_text(), "pinned\n")

    def test_existing_target_paths_and_options_are_preserved(self):
        for name in ("HAS1_itembox", "HAS1_generator", "HAS1_altar", "HAS1_duct"):
            directory, fqbn = TARGETS[name]
            self.assertEqual(directory, f"devices/{name}")
            self.assertEqual(fqbn, "esp32:esp32:ttgo-t1:UploadSpeed=115200,CPUFreq=240,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=min_spiffs,DebugLevel=none,EraseFlash=none")
        for name in ("HAS1_revival_machine", "HAS1_escape_main", "HAS1_tagmachine_main"):
            directory, fqbn = TARGETS[name]
            self.assertEqual(directory, f"devices/{name}")
            self.assertEqual(fqbn, "esp32:esp32:ttgo-t1:UploadSpeed=921600,CPUFreq=240,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=default,DebugLevel=none,EraseFlash=none")

    def test_nested_beetle_keeps_its_unique_release_tag(self):
        directory, fqbn = TARGETS["iotglove_beetle"]
        self.assertEqual(directory, "devices/iotglove/iotglove_beetle")
        self.assertEqual(Path(directory).name, "iotglove_beetle")
        self.assertIn(":esp32c3:", fqbn)
        self.assertIn("CDCOnBoot=cdc", fqbn)
        self.assertIn("PartitionScheme=min_spiffs", fqbn)
        self.assertEqual(len({Path(path).name for path, _ in TARGETS.values()}), len(TARGETS))

    def test_tagmachine_sub_uses_beetle_c3_release_target(self):
        directory, fqbn = TARGETS["HAS1_tagmachine_sub"]
        self.assertEqual(directory, "devices/HAS1_tagmachine_sub")
        self.assertEqual(Path(directory).name, "HAS1_tagmachine_sub")
        self.assertIn(":dfrobot_beetle_esp32c3:", fqbn)
        self.assertIn("CDCOnBoot=cdc", fqbn)
        self.assertIn("PartitionScheme=default", fqbn)
        self.assertEqual(len({Path(path).name for path, _ in TARGETS.values()}), len(TARGETS))

    def test_validation_cannot_copy_real_secret_or_beetle_into_ttgo(self):
        with tempfile.TemporaryDirectory() as work:
            source = Path(work) / "source"
            source.mkdir()
            (source / "iotglove.ino").write_text("void setup() {}\nvoid loop() {}\n")
            (source / "secrets.h").write_text("LOCAL_SECRET_MUST_STAY_UNCHANGED")
            (source / "iotglove_beetle").mkdir()
            (source / "iotglove_beetle" / "iotglove_beetle.ino").write_text("other board")
            (source / "update.bin").write_bytes(b"old deployed image")
            (source / "src").mkdir()
            (source / "src" / "driver.cpp").write_text("// driver\n")
            destination = Path(work) / "staged"
            glove_compile.stage_sketch(source, destination)
            self.assertEqual((source / "secrets.h").read_text(), "LOCAL_SECRET_MUST_STAY_UNCHANGED")
            self.assertIn("__COMPILE_ONLY_DO_NOT_DEPLOY__", (destination / "secrets.h").read_text())
            self.assertTrue((destination / "src" / "driver.cpp").is_file())
            self.assertFalse((destination / "iotglove_beetle").exists())
            self.assertFalse((destination / "update.bin").exists())

    def test_missing_or_placeholder_signing_keys_fail_before_deployment(self):
        for value in (None, "", "  ", "CHANGE_THIS_TO_YOUR_SECRET", " REPLACE_WITH_DEPLOYMENT_SECRET ", "__COMPILE_ONLY_DO_NOT_DEPLOY__", LINK_VALIDATION_SECRET, "bad\nkey"):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    checked_secret(value)
        self.assertEqual(
            checked_secret(LINK_VALIDATION_SECRET, allow_link_validation=True),
            LINK_VALIDATION_SECRET,
        )
        self.assertEqual(checked_secret('key-with-"quote-and-backslash\\'), 'key-with-"quote-and-backslash\\')


if __name__ == "__main__":
    unittest.main()
