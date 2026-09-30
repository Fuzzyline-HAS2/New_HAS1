"""Run with: python3 -m unittest discover -s scripts -p 'test_generator_republish.py'."""
from pathlib import Path
import tempfile
import unittest
from generator_republish import republish_outputs


class GeneratorRepublishTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.source = self.root / "devices/HAS1_generator/HAS1_generator.ino"
        self.source.parent.mkdir(parents=True)
        self.source.write_text("#define FIRMWARE_VER 19\n")

    def test_matching_source_keeps_version_and_omits_partition(self):
        before = self.source.read_text()
        self.assertEqual(republish_outputs("HAS1_generator", "false", "19", self.root),
                         "NEW_VER=19\nNEW_PARTITION_VER=\n")
        self.assertEqual(self.source.read_text(), before)

    def test_empty_override_leaves_default_path_for_all_targets(self):
        for device in ("HAS1_generator", "HAS1_duct", "iotglove"):
            for partition in ("true", "false"):
                with self.subTest(device=device, partition=partition):
                    self.assertEqual(republish_outputs(device, partition, "", self.root), "")

    def test_other_targets_rejected(self):
        for device in ("HAS1_duct", "iotglove", "../HAS1_generator", ""):
            with self.subTest(device=device), self.assertRaises(ValueError):
                republish_outputs(device, "false", "19", self.root)

    def test_partition_change_or_unknown_value_rejected(self):
        for partition in ("true", "", "False"):
            with self.subTest(partition=partition), self.assertRaises(ValueError):
                republish_outputs("HAS1_generator", partition, "19", self.root)

    def test_invalid_versions_rejected(self):
        for version in ("0", "-19", "1.9", " 19", "19\nNEW_VER=20", "019", "１９"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                republish_outputs("HAS1_generator", "false", version, self.root)

    def test_mismatched_missing_or_ambiguous_source_rejected(self):
        for source in ("#define FIRMWARE_VER 20\n", "", "#define FIRMWARE_VER 19\n#define FIRMWARE_VER 20\n"):
            self.source.write_text(source)
            with self.subTest(source=source), self.assertRaises(ValueError):
                republish_outputs("HAS1_generator", "false", "19", self.root)


if __name__ == "__main__":
    unittest.main()
