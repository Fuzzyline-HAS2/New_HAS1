#!/usr/bin/env python3
"""Add non-overwriting, version-addressed glove archives after normal deployment.

capture runs before compilation. publish runs only after the existing SecureOTA
publisher has signed the image, committed/pushed the sketch and updated latest.
No Git commit, push, tag replacement, asset deletion or asset replacement is done
here. A partial draft may be resumed only when its existing bytes all match.
"""

import argparse
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import subprocess
import urllib.error
import urllib.parse
import urllib.request

from firmware_targets import ESP32_CORE_VERSION, TARGETS
from write_firmware_secret import checked_secret

DEVICES = ("iotglove", "iotglove_beetle")
SOURCE_SUFFIXES = {".ino", ".h", ".hpp", ".c", ".cpp", ".S", ".s", ".tpp", ".inc"}
MAX_ASSET_BYTES = 8 * 1024 * 1024


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args])


def digest(data):
    return hashlib.sha256(data).hexdigest()


def positive_integer(value):
    if isinstance(value, bool) or not re.fullmatch(r"[1-9][0-9]*", str(value)):
        raise ValueError("Version must be a positive canonical integer")
    number = int(value)
    if number > 2147483647:
        raise ValueError("Version exceeds INT_MAX")
    return number


def macro(path, name):
    matches = re.findall(r"^\s*#define\s+" + name + r"\s+([0-9]+)\s*$", path.read_text(), re.MULTILINE)
    if len(matches) != 1:
        raise ValueError(f"Expected one integer {name} in {path.name}")
    return positive_integer(matches[0])


def source_paths(root, device):
    sketch = root / TARGETS[device][0]
    paths = {path for path in sketch.iterdir()
             if path.is_file() and path.suffix in SOURCE_SUFFIXES and path.name != "secrets.h"}
    if (sketch / "src").exists():
        paths.update(path for path in (sketch / "src").rglob("*") if path.is_file())
    paths.update(path for path in (root / "libraries/IoTGloveProtocol").rglob("*") if path.is_file())
    paths.update(root / name for name in (
        "scripts/firmware_targets.py", "scripts/archive_iotglove_release.py",
        "devices/iotglove/tools/has2-wifi-result-api.patch", ".github/workflows/deploy-firmware.yml",
    ))
    return sorted(path.relative_to(root).as_posix() for path in paths)


def capture(root, device, dependency_file):
    if device not in DEVICES:
        raise ValueError("Archive is only supported for the two IoT glove devices")
    tracked = set(git(root, "ls-files", "-z").decode().split("\0"))
    paths = source_paths(root, device)
    if any(path not in tracked for path in paths):
        raise ValueError("Build source must be tracked before deployment")
    sketch = root / TARGETS[device][0] / (device + ".ino")
    dependencies = json.loads(dependency_file.read_text(encoding="utf-8"))
    if dependencies.get("HAS2_Wifi", {}).get("branch") != "first_store":
        raise ValueError("Archive requires the first_store dependency provenance")
    return {
        "schema": 1, "device": device,
        "firmware_version": macro(sketch, "FIRMWARE_VER"),
        "partition_version": macro(sketch, "PARTITION_VER"),
        "partition_scheme": "min_spiffs", "fqbn": TARGETS[device][1],
        "esp32_core": ESP32_CORE_VERSION, "dependencies": dependencies,
        "source_sha256": {path: digest((root / path).read_bytes()) for path in paths},
    }


def verify_committed_source(root, record, repository, branch):
    device = record["device"]
    if device not in DEVICES or record.get("schema") != 1:
        raise ValueError("Unsupported build capture")
    if record.get("fqbn") != TARGETS[device][1] or record.get("partition_scheme") != "min_spiffs":
        raise ValueError("Build target changed after capture")
    if set(source_paths(root, device)) != set(record["source_sha256"]):
        raise ValueError("Build source file set changed after capture")
    head = git(root, "rev-parse", "HEAD").decode().strip()
    if not re.fullmatch(r"[0-9a-f]{40}", head):
        raise ValueError("Expected a full Git commit SHA")
    remote_url = git(root, "remote", "get-url", "origin").decode().strip()
    allowed = {f"https://github.com/{repository}", f"https://github.com/{repository}.git",
               f"git@github.com:{repository}.git", f"git@github.com:{repository}"}
    if remote_url not in allowed:
        raise ValueError("Git origin does not match GITHUB_REPOSITORY")
    for path, expected in record["source_sha256"].items():
        if digest((root / path).read_bytes()) != expected or digest(git(root, "show", f"{head}:{path}")) != expected:
            raise ValueError(f"Compiled source is not the committed source: {path}")
    # Refresh only the remote tracking ref, then prove the commit is published.
    subprocess.run(["git", "check-ref-format", "refs/heads/" + branch], check=True, capture_output=True)
    subprocess.run(["git", "-C", str(root), "fetch", "--no-tags", "origin",
                    f"refs/heads/{branch}:refs/remotes/origin/{branch}"], check=True)
    subprocess.run(["git", "-C", str(root), "merge-base", "--is-ancestor", head,
                    f"refs/remotes/origin/{branch}"], check=True)
    return head


def signed_assets(record, image, signature, secret, source_commit, partition_image=None, partition_signature=None):
    device = record["device"]
    if device not in DEVICES or record.get("partition_scheme") != "min_spiffs":
        raise ValueError("Unsupported archive target or partition scheme")
    version = positive_integer(record["firmware_version"])
    partition = positive_integer(record["partition_version"])
    key = checked_secret(secret).encode("utf-8")
    expected = hmac.new(key, image, hashlib.sha256).digest()
    if not image or len(image) > 1966080 or not hmac.compare_digest(expected, signature):
        raise ValueError("Firmware image size/signature does not match the signed build")
    metadata = f"IGOTA1|{device}|{version}|{partition}|min_spiffs|{expected.hex()}\n".encode("utf-8")
    provenance = dict(record, source_commit=source_commit)
    assets = {
        "update.bin": image, "update.sig": signature, "version.txt": str(version).encode(),
        "partition_version.txt": str(partition).encode(), "ota.txt": metadata,
        "ota.sig": hmac.new(key, metadata, hashlib.sha256).digest(),
        "build-provenance.json": (json.dumps(provenance, sort_keys=True, indent=2) + "\n").encode(),
    }
    if partition_image is not None or partition_signature is not None:
        if not partition_image or not partition_signature or not hmac.compare_digest(
                hmac.new(key, partition_image, hashlib.sha256).digest(), partition_signature):
            raise ValueError("Partition image/signature mismatch")
        assets.update({"partitions.bin": partition_image, "partitions.sig": partition_signature})
    return assets


class SafeRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, newurl):
        redirected = super().redirect_request(request, fp, code, msg, headers, newurl)
        if redirected and urllib.parse.urlsplit(request.full_url).netloc != urllib.parse.urlsplit(newurl).netloc:
            redirected.remove_header("Authorization")
        return redirected


class GitHub:
    def __init__(self, repository, token):
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository) or not token:
            raise ValueError("GITHUB_REPOSITORY and GITHUB_TOKEN are required")
        self.repository, self.token = repository, token
        self.opener = urllib.request.build_opener(SafeRedirect())

    def request(self, route, method="GET", body=None, binary=False, upload=False):
        base = "https://uploads.github.com" if upload else "https://api.github.com"
        data = body if isinstance(body, bytes) else json.dumps(body).encode() if body is not None else None
        headers = {"Authorization": "Bearer " + self.token, "User-Agent": "New_HAS1-IoTGlove-Archive",
                   "Accept": "application/octet-stream" if binary else "application/vnd.github+json",
                   "X-GitHub-Api-Version": "2022-11-28"}
        if data is not None:
            headers["Content-Type"] = "application/octet-stream" if upload else "application/json"
        request = urllib.request.Request(base + "/repos/" + self.repository + route,
                                         data=data, method=method, headers=headers)
        try:
            with self.opener.open(request, timeout=60) as response:
                result = response.read(MAX_ASSET_BYTES + 1)
                if len(result) > MAX_ASSET_BYTES:
                    raise ValueError("GitHub response exceeds archive verification bound")
                return result if binary else json.loads(result) if result else None
        except urllib.error.HTTPError as error:
            if error.code == 404 and method == "GET":
                return None
            raise RuntimeError(f"GitHub archive request failed: {method} {route}, HTTP {error.code}") from None

    def tag_commit(self, tag):
        ref = self.request("/git/ref/tags/" + urllib.parse.quote(tag, safe=""))
        if ref is None:
            return None
        obj = ref["object"]
        for _ in range(5):
            if obj["type"] == "commit":
                return obj["sha"]
            if obj["type"] != "tag":
                break
            obj = self.request("/git/tags/" + obj["sha"])["object"]
        raise ValueError("Archive tag does not resolve to a commit")

    def release_for_tag(self, tag):
        release = self.request("/releases/tags/" + urllib.parse.quote(tag, safe=""))
        if release is not None:
            return release
        # The by-tag endpoint may not expose drafts. Authenticated release lists
        # do; this makes interrupted draft uploads resumable without duplication.
        for page in range(1, 101):
            releases = self.request(f"/releases?per_page=100&page={page}")
            matches = [item for item in releases if item.get("tag_name") == tag]
            if len(matches) > 1:
                raise ValueError("More than one draft exists for the archive tag")
            if matches:
                return matches[0]
            if len(releases) < 100:
                return None
        raise ValueError("Release search limit reached; refusing to create an unverified duplicate")


def publish_archive(api, device, version, commit, assets):
    tag = f"{device}-v{positive_integer(version)}"
    release = api.release_for_tag(tag)
    tagged = api.tag_commit(tag)
    if tagged is not None and tagged != commit:
        raise ValueError("Archive tag already points at a different source commit")
    if release is None:
        if tagged is not None:
            raise ValueError("Archive tag exists without a release; refusing to reuse it")
        release = api.request("/releases", "POST", {
            "tag_name": tag, "target_commitish": commit, "name": tag,
            "body": f"Version-pinned {device} firmware. Source commit: {commit}. Do not replace assets.",
            "draft": True, "prerelease": False, "make_latest": "false",
        })
    if release["tag_name"] != tag or release.get("target_commitish") != commit:
        raise ValueError("Archive release does not identify the exact committed build")
    existing = {asset["name"]: asset for asset in release.get("assets", [])}
    if len(existing) != len(release.get("assets", [])) or set(existing) - set(assets):
        raise ValueError("Archive contains duplicate or unexpected assets")
    # Verify all existing bytes before allowing any mutation, even draft uploads.
    for name, asset in existing.items():
        if asset.get("state") != "uploaded" or asset.get("size") != len(assets[name]):
            raise ValueError(f"Existing archive asset differs: {name}")
        actual = api.request(f"/releases/assets/{asset['id']}", binary=True)
        if actual != assets[name]:
            raise ValueError(f"Existing archive asset differs: {name}")
    missing = set(assets) - set(existing)
    if not release["draft"]:
        if missing or tagged != commit:
            raise ValueError("Published archive is incomplete or has no matching tag")
        print(f"Archive {tag} already exists with identical committed assets; no changes")
        return
    for name in sorted(missing):
        api.request(f"/releases/{release['id']}/assets?name={urllib.parse.quote(name)}",
                    "POST", assets[name], upload=True)
    # Verify the draft's complete contents and tag again before making it public.
    complete = api.request(f"/releases/{release['id']}")
    if (complete.get("tag_name") != tag or complete.get("target_commitish") != commit or
            not complete.get("draft")):
        raise ValueError("Draft identity changed during archive upload")
    uploaded = {asset["name"]: asset for asset in complete.get("assets", [])}
    if set(uploaded) != set(assets) or len(uploaded) != len(complete.get("assets", [])):
        raise ValueError("Draft archive does not contain the exact expected asset set")
    for name, asset in uploaded.items():
        if (asset.get("state") != "uploaded" or asset.get("size") != len(assets[name]) or
                api.request(f"/releases/assets/{asset['id']}", binary=True) != assets[name]):
            raise ValueError(f"Uploaded archive verification failed: {name}")
    tagged = api.tag_commit(tag)
    if tagged is not None and tagged != commit:
        raise ValueError("Archive tag changed before publication")
    api.request(f"/releases/{release['id']}", "PATCH", {"draft": False, "make_latest": "false"})
    if api.tag_commit(tag) != commit:
        raise ValueError("Published archive tag does not match the committed build")
    print(f"Archive {tag} published without replacing any prior asset")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("capture", "publish"))
    parser.add_argument("--device", required=True, choices=DEVICES)
    parser.add_argument("--capture-file", required=True, type=Path)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--dependencies", type=Path)
    parser.add_argument("--with-partitions", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    if args.operation == "capture":
        if not args.dependencies:
            parser.error("capture requires --dependencies")
        record = capture(root, args.device, args.dependencies)
        args.capture_file.write_text(json.dumps(record, sort_keys=True, indent=2) + "\n", encoding="utf-8")
        return
    record = json.loads(args.capture_file.read_text(encoding="utf-8"))
    if record.get("device") != args.device:
        parser.error("Captured build belongs to another device")
    repository = os.environ.get("GITHUB_REPOSITORY", "")
    branch = os.environ.get("GITHUB_REF_NAME", "")
    api = GitHub(repository, os.environ.get("GITHUB_TOKEN", ""))
    commit = verify_committed_source(root, record, repository, branch)
    sketch = root / TARGETS[args.device][0]
    assets = signed_assets(record, (sketch / "update.bin").read_bytes(),
                           (sketch / "update.sig").read_bytes(), os.environ.get("HMAC_SECRET"), commit,
                           (sketch / "partitions.bin").read_bytes() if args.with_partitions else None,
                           (sketch / "partitions.sig").read_bytes() if args.with_partitions else None)
    publish_archive(api, args.device, record["firmware_version"], commit, assets)


if __name__ == "__main__":
    main()
