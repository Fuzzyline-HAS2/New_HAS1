"""Version-pinned archive protocol and failure/retry behavior; no network calls."""

import copy
import hashlib
import hmac
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import urllib.request

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
import archive_iotglove_release as archive  # noqa: E402

COMMIT = "a" * 40
SECRET = "test-key-used-only-by-offline-unit-tests"


def record():
    return {
        "schema": 1, "device": "iotglove", "firmware_version": 12,
        "partition_version": 1, "partition_scheme": "min_spiffs",
        "fqbn": archive.TARGETS["iotglove"][1], "esp32_core": "3.3.11",
        "dependencies": {"HAS2_Wifi": {"branch": "first_store", "commit": "b" * 40}},
        "source_sha256": {},
    }


def assets():
    image = b"test-firmware-binary"
    signature = hmac.new(SECRET.encode(), image, hashlib.sha256).digest()
    return archive.signed_assets(record(), image, signature, SECRET, COMMIT)


class FakeGitHub:
    def __init__(self):
        self.release = None
        self.tag = None
        self.data = {}
        self.mutations = []
        self.fail_upload = None

    def tag_commit(self, tag):
        return self.tag

    def release_for_tag(self, tag):
        return copy.deepcopy(self.release)

    def request(self, route, method="GET", body=None, binary=False, upload=False):
        if method != "GET":
            self.mutations.append((method, route))
        if route == "/releases" and method == "POST":
            assert self.release is None
            self.release = dict(body, id=7, assets=[])
            return copy.deepcopy(self.release)
        if route.startswith("/releases/7/assets?name=") and method == "POST":
            name = route.split("name=", 1)[1]
            if self.fail_upload == name:
                raise RuntimeError("simulated interrupted upload")
            assert upload and name not in [item["name"] for item in self.release["assets"]]
            identifier = len(self.data) + 1
            self.data[identifier] = body
            self.release["assets"].append({"id": identifier, "name": name, "state": "uploaded", "size": len(body)})
            return {}
        if route.startswith("/releases/assets/") and method == "GET":
            assert binary
            return self.data[int(route.rsplit("/", 1)[1])]
        if route == "/releases/7" and method == "GET":
            return copy.deepcopy(self.release)
        if route == "/releases/7" and method == "PATCH":
            assert body == {"draft": False, "make_latest": "false"}
            self.release.update(body)
            self.tag = self.release["target_commitish"]
            return copy.deepcopy(self.release)
        raise AssertionError((route, method))


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        # Fake API operations must not appear as actual publication in test logs.
        quiet = patch("builtins.print")
        quiet.start()
        self.addCleanup(quiet.stop)

    def test_signed_metadata_binds_device_versions_partition_and_image(self):
        output = assets()
        image_hmac = hmac.new(SECRET.encode(), output["update.bin"], hashlib.sha256).hexdigest()
        expected = f"IGOTA1|iotglove|12|1|min_spiffs|{image_hmac}\n".encode()
        self.assertEqual(output["ota.txt"], expected)
        self.assertEqual(output["ota.sig"], hmac.new(SECRET.encode(), expected, hashlib.sha256).digest())
        self.assertEqual(len(output["ota.sig"]), 32)
        self.assertEqual(output["version.txt"], b"12")
        self.assertEqual(output["partition_version.txt"], b"1")
        self.assertNotIn(SECRET.encode(), output["build-provenance.json"])
        self.assertEqual(json.loads(output["build-provenance.json"])["source_commit"], COMMIT)

    def test_wrong_image_signature_or_partition_signature_is_rejected(self):
        output = assets()
        with self.assertRaises(ValueError):
            archive.signed_assets(record(), b"different-image", output["update.sig"], SECRET, COMMIT)
        with self.assertRaises(ValueError):
            archive.signed_assets(record(), output["update.bin"], output["update.sig"], SECRET, COMMIT, b"partition", b"bad")
        partition = b"test-partition"
        signature = hmac.new(SECRET.encode(), partition, hashlib.sha256).digest()
        output = archive.signed_assets(record(), output["update.bin"], output["update.sig"], SECRET, COMMIT, partition, signature)
        self.assertEqual(output["partitions.bin"], partition)
        self.assertEqual(output["partitions.sig"], signature)

    def test_version_and_board_contract_is_strict(self):
        for value in (True, 0, -1, "01", "1:2", 2147483648):
            with self.subTest(value=value), self.assertRaises(ValueError):
                archive.positive_integer(value)
        self.assertEqual(archive.positive_integer(2147483647), 2147483647)
        invalid = record()
        invalid["device"] = "HAS1_duct"
        with self.assertRaises(ValueError):
            archive.signed_assets(invalid, b"x", b"y", SECRET, COMMIT)

    def test_create_then_exact_retry_never_overwrites(self):
        api = FakeGitHub()
        output = assets()
        archive.publish_archive(api, "iotglove", 12, COMMIT, output)
        self.assertFalse(api.release["draft"])
        self.assertEqual(api.release["target_commitish"], COMMIT)
        self.assertEqual(api.release["make_latest"], "false")
        before = list(api.mutations)
        archive.publish_archive(api, "iotglove", 12, COMMIT, output)
        self.assertEqual(api.mutations, before)
        self.assertFalse(any(method == "DELETE" for method, _ in api.mutations))

    def test_interrupted_draft_resumes_only_missing_assets(self):
        api = FakeGitHub()
        api.fail_upload = "update.bin"
        output = assets()
        with self.assertRaises(RuntimeError):
            archive.publish_archive(api, "iotglove", 12, COMMIT, output)
        self.assertTrue(api.release["draft"])
        before = {item["name"]: item["id"] for item in api.release["assets"]}
        self.assertTrue(before)
        api.fail_upload = None
        archive.publish_archive(api, "iotglove", 12, COMMIT, output)
        after = {item["name"]: item["id"] for item in api.release["assets"]}
        self.assertTrue(all(after[name] == identifier for name, identifier in before.items()))
        self.assertFalse(api.release["draft"])

    def test_conflicting_existing_archive_has_no_mutations(self):
        api = FakeGitHub()
        output = assets()
        archive.publish_archive(api, "iotglove", 12, COMMIT, output)
        first_id = api.release["assets"][0]["id"]
        api.data[first_id] = b"changed-byte-content"
        api.mutations.clear()
        with self.assertRaises(ValueError):
            archive.publish_archive(api, "iotglove", 12, COMMIT, output)
        self.assertEqual(api.mutations, [])
        api.tag = "c" * 40
        with self.assertRaises(ValueError):
            archive.publish_archive(api, "iotglove", 12, COMMIT, output)
        self.assertEqual(api.mutations, [])

    def test_orphan_tag_and_incomplete_published_archive_fail_closed(self):
        api = FakeGitHub()
        api.tag = COMMIT
        with self.assertRaises(ValueError):
            archive.publish_archive(api, "iotglove", 12, COMMIT, assets())
        self.assertEqual(api.mutations, [])
        api.tag = None
        archive.publish_archive(api, "iotglove", 12, COMMIT, assets())
        api.release["assets"].pop()
        api.mutations.clear()
        with self.assertRaises(ValueError):
            archive.publish_archive(api, "iotglove", 12, COMMIT, assets())
        self.assertEqual(api.mutations, [])

    def test_source_must_match_committed_build_before_remote_mutation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "main.cpp").write_bytes(b"compiled-source")
            captured = record()
            captured["source_sha256"] = {"main.cpp": archive.digest(b"compiled-source")}

            def fake_git(root, *args):
                if args == ("rev-parse", "HEAD"): return COMMIT.encode()
                if args == ("remote", "get-url", "origin"): return b"https://github.com/owner/repo.git"
                if args == ("show", COMMIT + ":main.cpp"): return b"compiled-source"
                raise AssertionError(args)

            with patch.object(archive, "source_paths", return_value=["main.cpp"]), patch.object(archive, "git", side_effect=fake_git), patch.object(archive.subprocess, "run") as command:
                self.assertEqual(archive.verify_committed_source(root, captured, "owner/repo", "main"), COMMIT)
                self.assertIn("fetch", command.call_args_list[1].args[0])
                self.assertIn("--is-ancestor", command.call_args_list[2].args[0])
                command.reset_mock()
                (root / "main.cpp").write_bytes(b"changed-after-compile")
                with self.assertRaises(ValueError):
                    archive.verify_committed_source(root, captured, "owner/repo", "main")
                command.assert_not_called()

    def test_draft_lookup_when_by_tag_endpoint_cannot_find_it(self):
        api = archive.GitHub("owner/repo", "test-token")
        draft = {"tag_name": "iotglove-v12", "draft": True}
        with patch.object(api, "request", side_effect=[None, [draft]]):
            self.assertEqual(api.release_for_tag("iotglove-v12"), draft)

    def test_asset_redirect_never_forwards_token_to_storage_host(self):
        request = urllib.request.Request("https://api.github.com/repos/a/b/releases/assets/1", headers={"Authorization": "Bearer test-token"})
        redirected = archive.SafeRedirect().redirect_request(request, None, 302, "Found", {}, "https://release-assets.githubusercontent.com/download")
        self.assertIsNone(redirected.get_header("Authorization"))
        redirected = archive.SafeRedirect().redirect_request(request, None, 302, "Found", {}, "https://api.github.com/redirected")
        self.assertEqual(redirected.get_header("Authorization"), "Bearer test-token")


if __name__ == "__main__":
    unittest.main()
