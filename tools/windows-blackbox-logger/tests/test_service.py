"""Service contracts that can run without installing anything in Windows SCM."""
from contextlib import closing
import json
import os
from pathlib import Path
import queue
import sqlite3
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from collector import Collector
from service_definition import SERVICE_ID, WINSW_SHA256, service_xml
from service_runtime import BrokerInstanceLock, InstanceLock, read_machine_password
from helpers import TOPIC, payload


class WindowsScriptEntryTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "Windows PowerShell 5.1 -File parameter binding")
    def test_status_without_config_from_unrelated_directory(self):
        # Read-only status must bind without a caller-supplied Config or script
        # context. WinPS 5.1 left PSScriptRoot empty in the old parameter default.
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([
                "powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                "-File", str(ROOT / "service.ps1"), "status",
            ], cwd=directory, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("HAS1BlackBoxCollector", result.stdout)


class LockTests(unittest.TestCase):
    def test_other_process_refused_then_crash_releases_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            database = Path(directory) / "blackbox.sqlite3"
            script = """
import sys,time
from service_runtime import InstanceLock
with InstanceLock(sys.argv[1]):
    print('locked', flush=True)
    time.sleep(60)
"""
            process = subprocess.Popen([sys.executable, "-c", script, str(database)], cwd=ROOT,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                output = queue.Queue()
                reader = threading.Thread(target=lambda: output.put(process.stdout.readline()), daemon=True)
                reader.start()
                self.assertEqual(output.get(timeout=10).strip(), "locked")
                with self.assertRaisesRegex(RuntimeError, "already owns"):
                    with InstanceLock(database):
                        self.fail("duplicate lock accepted")
            finally:
                process.kill()
                process.communicate(timeout=10)
            with InstanceLock(database):
                self.assertTrue(Path(str(database) + ".lock").exists())
            self.assertFalse(database.exists())

    def test_normal_exit_unlocks_without_deleting_database(self):
        with tempfile.TemporaryDirectory() as directory:
            database = Path(directory) / "blackbox.sqlite3"
            database.write_bytes(b"preserve me")
            with InstanceLock(database):
                pass
            with InstanceLock(database):
                self.assertEqual(database.read_bytes(), b"preserve me")


class BrokerLockTests(unittest.TestCase):
    def test_identity_normalizes_hostname_but_not_client_id(self):
        base = {"host": "BROKER.Example.", "port": 1883, "client_id": "HAS1"}
        self.assertEqual(BrokerInstanceLock(base).name,
                         BrokerInstanceLock(dict(base, host="broker.example")).name)
        self.assertNotEqual(BrokerInstanceLock(base).name,
                            BrokerInstanceLock(dict(base, client_id="has1")).name)
        self.assertNotEqual(BrokerInstanceLock(base).name,
                            BrokerInstanceLock(dict(base, port=1884)).name)

    @unittest.skipUnless(os.name == "nt", "Windows global cross-session mutex")
    def test_separate_process_same_broker_different_database_refused(self):
        import uuid
        config = {"host": "127.0.0.1", "port": 1883, "client_id": uuid.uuid4().hex}
        script = """
import sys,json
from service_runtime import BrokerInstanceLock
try:
    with BrokerInstanceLock(json.loads(sys.argv[1])):
        raise SystemExit(0)
except RuntimeError:
    raise SystemExit(23)
"""
        with BrokerInstanceLock(config):
            result = subprocess.run([sys.executable, "-c", script, json.dumps(config)], cwd=ROOT, timeout=10)
            self.assertEqual(result.returncode, 23)
        result = subprocess.run([sys.executable, "-c", script, json.dumps(config)], cwd=ROOT, timeout=10)
        self.assertEqual(result.returncode, 0)


class ShutdownTests(unittest.TestCase):
    def test_close_drains_accepted_and_rejects_new_batch_before_commit_ack(self):
        with tempfile.TemporaryDirectory() as directory:
            database = Path(directory) / "blackbox.sqlite3"
            entered, release = threading.Event(), threading.Event()
            acks = []
            from storage import Store
            class PausedStore(Store):
                def save(self, batch, **timing):
                    entered.set()
                    if not release.wait(5):
                        raise RuntimeError("test writer was not released")
                    return super().save(batch, **timing)
            def publish(topic, body):
                with closing(sqlite3.connect(database)) as reader:
                    self.assertGreater(reader.execute("SELECT count(*) FROM records").fetchone()[0], 0)
                acks.append(body)
                return True
            collector = Collector(database, publish, store_factory=PausedStore)
            collector.start()
            self.assertTrue(collector.submit(TOPIC, payload((1,))))
            self.assertTrue(entered.wait(5))
            self.assertTrue(collector.submit(TOPIC, payload((2,))))
            closer = threading.Thread(target=collector.close)
            closer.start()
            try:
                self.assertTrue(collector.stop_event.wait(5))
                self.assertFalse(collector.submit(TOPIC, payload((3,))))
                self.assertTrue(closer.is_alive())
                self.assertEqual(acks, [])
            finally:
                release.set()
                closer.join(timeout=5)
            self.assertFalse(closer.is_alive())
            self.assertEqual(len(acks), 2)
            self.assertEqual(collector.pending.unfinished_tasks, 0)
            with closing(sqlite3.connect(database)) as reader:
                self.assertEqual(reader.execute("SELECT count(*) FROM records").fetchone()[0], 2)

    def test_acceptance_finishes_before_close_can_stop_writer(self):
        # A submit paused between its state check and queue insertion must either
        # be rejected or be drained; never be stranded after close returns.
        entered, release = threading.Event(), threading.Event()
        class PausedQueue(queue.Queue):
            def put_nowait(self, value):
                entered.set()
                if not release.wait(5):
                    raise RuntimeError("test enqueue was not released")
                return super().put_nowait(value)
        with tempfile.TemporaryDirectory() as directory:
            collector = Collector(Path(directory) / "test.sqlite3", lambda *_: True)
            collector.pending = PausedQueue(maxsize=64)
            collector.start()
            accepted = []
            submitter = threading.Thread(target=lambda: accepted.append(collector.submit(TOPIC, payload())))
            submitter.start()
            self.assertTrue(entered.wait(5))
            closer = threading.Thread(target=collector.close)
            closer.start()
            release.set()
            submitter.join(timeout=5)
            closer.join(timeout=5)
            self.assertEqual(accepted, [True])
            self.assertFalse(closer.is_alive())
            self.assertEqual(collector.counts["committed_batches"], 1)


class DefinitionTests(unittest.TestCase):
    def test_definition_escapes_paths_and_has_bounded_default_stop(self):
        app = Path("C:/Program Files/HAS1 & BlackBox")
        state = Path("C:/ProgramData/HAS1 & BlackBox")
        document = ET.fromstring(service_xml(app, state))
        self.assertEqual(document.findtext("id"), SERVICE_ID)
        self.assertEqual(document.findtext("workingdirectory"), str(app))
        self.assertEqual(document.findtext("executable"), str(app / ".venv/Scripts/python.exe"))
        self.assertEqual(document.findtext("hidewindow"), "false")
        self.assertEqual(document.findtext("stoptimeout"), "30 sec")
        self.assertEqual(document.findtext("stopparentprocessfirst"), "true")
        self.assertIsNone(document.find("stoparguments"))
        self.assertIsNone(document.find("stopexecutable"))
        self.assertEqual(document.findtext("startmode"), "Automatic")
        self.assertIsNotNone(document.find("delayedAutoStart"))
        self.assertEqual(document.findtext("serviceaccount/user"), "LocalService")
        self.assertEqual([item.attrib for item in document.findall("onfailure")], [
            {"action": "restart", "delay": "10 sec"},
            {"action": "restart", "delay": "30 sec"},
            {"action": "restart", "delay": "60 sec"}])
        self.assertTrue(document.findtext("arguments").startswith("-E -s -u "))
        self.assertIn("--credential-file", document.findtext("arguments"))
        self.assertNotIn("HAS1_BLACKBOX_PASSWORD", service_xml(app, state))
        self.assertIsNone(document.find("serviceaccount/password"))

    def test_installer_pin_matches_definition(self):
        script = (ROOT / "service.ps1").read_text()
        self.assertIn(WINSW_SHA256, script)
        self.assertIn("releases/download/v2.12.0/WinSW.NET461.exe", script)

    def test_missing_config_check_does_not_create_database(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([sys.executable, str(ROOT / "collector.py"), "--check-config",
                                     "--config", str(Path(directory) / "missing.json")],
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(list(Path(directory).iterdir()), [])

    @unittest.skipIf(os.name == "nt", "non-Windows behavior")
    def test_dpapi_is_not_silently_replaced_with_environment(self):
        with patch.dict(os.environ, {"HAS1_BLACKBOX_PASSWORD": "unrelated"}):
            with self.assertRaisesRegex(RuntimeError, "require Windows"):
                read_machine_password("missing.dpapi")

    @unittest.skipUnless(os.name == "nt", "Windows machine DPAPI")
    def test_real_machine_dpapi_roundtrip_and_corruption(self):
        # No password appears in command arguments. PowerShell and Python share
        # an ephemeral protected blob only; no SCM installation is performed.
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "credential.dpapi"
            code = """
Add-Type -AssemblyName System.Security
$bytes = [Text.Encoding]::UTF8.GetBytes('test-password-' + [char]0xAC00)
[IO.File]::WriteAllBytes($args[0], [Security.Cryptography.ProtectedData]::Protect(
    $bytes,$null,[Security.Cryptography.DataProtectionScope]::LocalMachine))
"""
            script = Path(directory) / "protect.ps1"
            script.write_text(code)
            subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                            str(script), str(path)], check=True, timeout=30, capture_output=True)
            self.assertEqual(read_machine_password(path), "test-password-가")
            path.write_bytes(b"not dpapi")
            with self.assertRaises(OSError):
                read_machine_password(path)


if __name__ == "__main__":
    unittest.main()
