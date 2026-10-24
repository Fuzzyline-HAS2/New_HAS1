"""Fail-closed deploy settings, credential isolation, and old-target compatibility."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
from write_blackbox_release_config import configuration, checked_broker, checked_password
from firmware_targets import TARGETS
from archive_iotglove_release import source_paths, DEVICES


class BlackboxReleaseTests(unittest.TestCase):
    def test_mqtt_requires_each_setting_and_supported_target(self):
        for target in ("iotglove", "HAS1_generator"):
            for broker, password in ((None, None), ("mqtt://172.30.1.43:1883", None),
                                     (None, "a-real-deployment-secret")):
                with self.subTest(target=target, broker=broker), self.assertRaises(ValueError):
                    configuration(target, "mqtt", broker, password)
        for target in set(TARGETS) - {"iotglove", "HAS1_generator"}:
            with self.subTest(target=target), self.assertRaises(ValueError):
                configuration(target, "mqtt", "mqtt://172.30.1.43:1883", "deployment-password")

    def test_flash_does_not_embed_unused_credentials_for_any_target(self):
        for target in TARGETS:
            defines, public = configuration(target, "flash", "bad-uri", "should-never-appear")
            self.assertNotIn("should-never-appear", defines + json.dumps(public))
            self.assertEqual(public["broker_uri"], "")
            if target not in ("iotglove", "HAS1_generator"):
                self.assertEqual(defines, "")
                self.assertEqual(public["mode"], "not_applicable")

    def test_only_explicit_canonical_private_ipv4_brokers(self):
        for value in ("mqtt://172.30.1.43:1883", "mqtt://10.0.0.2:1", "mqtt://192.168.1.2:65535"):
            self.assertEqual(checked_broker(value), value)
        for value in ("", "mqtt://host:1883", "mqtt://127.0.0.1:1883", "mqtt://0.0.0.0:1883",
                      "mqtt://192.0.2.2:1883", "mqtt://239.1.1.1:1883", "mqtt://8.8.8.8:1883",
                      "mqtt://172.30.1.43:0", "mqtt://172.30.1.43:65536", "mqtt://172.30.1.43:01883",
                      "mqtt://172.030.1.43:1883", "mqtts://172.30.1.43:1883", "mqtt://172.30.1.43",
                      "mqtt://user:password@172.30.1.43:1883", "mqtt://172.30.1.43:1883/path",
                      "mqtt://172.30.1.43:1883?x=1", "mqtt://172.30.1.43:1883\n"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                checked_broker(value)

    def test_password_limits_and_placeholder_rejection(self):
        for value in ("", " ", "a" * 128, "x\n", "x\0", "x\x7f", "암호",
                      "REPLACE_WITH_PASSWORD", "test-only-password", "__COMPILE_ONLY_DO_NOT_DEPLOY__"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                checked_password(value)
        self.assertEqual(checked_password("a" * 127), "a" * 127)

    def run_helper(self, root, target, mode, broker="mqtt://172.30.1.43:1883", password='secure-quote"slash\\value'):
        directory = root / TARGETS[target][0]
        directory.mkdir(parents=True, exist_ok=True)
        header = directory / "secrets.h"
        header.write_text("existing-header")
        public = root / "public.json"
        env = dict(os.environ, HMAC_SECRET="existing-hmac-signing-key", BLACKBOX_BROKER_URI=broker,
                   BLACKBOX_MQTT_PASSWORD=password)
        process = subprocess.run([sys.executable, str(ROOT / "scripts/write_blackbox_release_config.py"),
                                  "--device", target, "--mode", mode, "--root", str(root),
                                  "--public-config", str(public)], env=env, capture_output=True, text=True)
        if password:
            self.assertNotIn(password, process.stdout + process.stderr)
        self.assertNotIn(env["HMAC_SECRET"], process.stdout + process.stderr)
        return process, header, public

    def test_cli_keeps_hmac_exact_and_public_metadata_credential_free(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for target, prefix in (("iotglove", "IOTGLOVE"), ("HAS1_generator", "HAS1_GENERATOR")):
                process, header, public = self.run_helper(root, target, "mqtt")
                self.assertEqual(process.returncode, 0, process.stderr)
                macros = {line.split(" ", 2)[1]: json.loads(line.split(" ", 2)[2])
                          for line in header.read_text().splitlines() if line.startswith("#define ")}
                self.assertEqual(macros, {"HMAC_SECRET": "existing-hmac-signing-key",
                                         prefix + "_BLACKBOX_BROKER_URI": "mqtt://172.30.1.43:1883",
                                         prefix + "_BLACKBOX_MQTT_PASSWORD": 'secure-quote"slash\\value'})
                self.assertEqual(json.loads(public.read_text()), {"schema": 1, "device": target,
                                 "mode": "mqtt", "broker_uri": "mqtt://172.30.1.43:1883",
                                 "authentication": "device_mac"})

    def test_invalid_mqtt_leaves_header_untouched_and_no_public_file(self):
        with tempfile.TemporaryDirectory() as directory:
            process, header, public = self.run_helper(Path(directory), "iotglove", "mqtt", password="")
            self.assertNotEqual(process.returncode, 0)
            self.assertEqual(header.read_text(), "existing-header")
            self.assertFalse(public.exists())

    def test_source_archive_hashes_helpers_but_never_secret_headers(self):
        for target in DEVICES:
            paths = source_paths(ROOT, target)
            self.assertIn("scripts/write_blackbox_release_config.py", paths)
            self.assertIn("scripts/write_firmware_secret.py", paths)
            self.assertFalse(any(Path(path).name == "secrets.h" for path in paths))


if __name__ == "__main__":
    unittest.main()
