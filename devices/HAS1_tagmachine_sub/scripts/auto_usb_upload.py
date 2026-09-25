#!/usr/bin/env python3
"""Download and upload an exact TagMachine Beetle versioned Release image.

Automatic mode deliberately ignores serial ports that were already present
when the script started. Connect exactly one Beetle when prompted. Use
``--port`` only when intentionally flashing an already-connected board.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request


DEVICE = "HAS1_tagmachine_sub"
REPOSITORY = "Fuzzyline-HAS2/New_HAS1"
API_BASE = f"https://api.github.com/repos/{REPOSITORY}"
CORE_VERSION = "3.3.11"
ESPTOOL_VERSION = "5.3.1"
PARTITION_VERSION = 1
PARTITION_SCHEME = "default"
SECUREOTA_REVISION = "162db758e6806895ef10ca39b101f9f8753bdc20"
RELEASE_FQBN = (
    "esp32:esp32:dfrobot_beetle_esp32c3:UploadSpeed=115200,"
    "CDCOnBoot=cdc,CPUFreq=160,FlashFreq=80,FlashMode=qio,FlashSize=4M,"
    "PartitionScheme=default,DebugLevel=none,EraseFlash=none"
)
UPLOAD_FQBN = RELEASE_FQBN.replace("UploadSpeed=115200", "UploadSpeed=460800")
FQBN = UPLOAD_FQBN  # Public alias used by the host-side tests.
SLOT_SIZE = 0x140000
REQUIRED_SLOT_MARGIN = 32 * 1024
MAX_IMAGE_BYTES = SLOT_SIZE - REQUIRED_SLOT_MARGIN
MAX_API_BYTES = 1024 * 1024
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_SIZE = 0xC00
OTA_DATA_OFFSET = 0xE000
OTA_DATA_SIZE = 0x2000
DEFAULT_PARTITION_SHA256 = (
    "53b91dac6e7a4dc14ab69656f0cc13902a1526f5a19d171ba9a526c005200899"
)
BOOT_APP0_SHA256 = (
    "f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0"
)
POLL_SECONDS = 0.5
STABLE_POLLS = 3
DISCONNECT_STABLE_SECONDS = 2.0
EXPECTED_ASSETS = (
    "update.bin",
    "update.sig",
    "ota.txt",
    "ota.sig",
    "version.txt",
    "partition_version.txt",
    "build-provenance.json",
)
ASSET_SIZE_LIMITS = {
    "update.bin": (24, MAX_IMAGE_BYTES),
    "update.sig": (32, 32),
    "ota.txt": (1, 512),
    "ota.sig": (32, 32),
    "version.txt": (1, 16),
    "partition_version.txt": (1, 16),
    "build-provenance.json": (2, MAX_API_BYTES),
}
# GitHub reports v4 as mutable, so API metadata alone is not a permanent trust
# anchor. Every allowed release must be reviewed and pinned here before use.
PINNED_RELEASES = {
    4: {
        "source_commit": "05ad44710616cf9c642c858c1567f56cbee4159f",
        "assets": {
            "update.bin": "027d456a056145ed1919285d9658c3adb7374f16504e67c504286a2786f53675",
            "update.sig": "2a44d02943e0caefe2efcdeaa074abf54391ff237c490f1bc010838bf8b61d79",
            "ota.txt": "dd5606d9a5e254be9c0d1d207bbf6ceda596d134a46664e9b0e75e2867c6abe2",
            "ota.sig": "dcac4a8f84174c540badd8aff6f1cf245b7fc2016fc2605e42dcd2ffd6ada160",
            "version.txt": "4b227777d4dd1fc61c6f884f48641d02b4d121d3fd328cb08b5531fcacdabf8a",
            "partition_version.txt": "6b86b273ff34fce19d6b804eff5a3f5747ada4eaa22f1d49c01e52ddb7875b4b",
            "build-provenance.json": "b5a284bffdbd8100d5f43a7ce6eb247f9cf2f5a0d08b2f686f917b8cb2ab1141",
        },
    }
}
# DFRobot's Beetle ESP32-C3 board exposes its CH343 USB-UART bridge with this
# generic WCH identity. It is not globally unique to Beetle hardware, so auto
# mode still requires the operator to connect exactly one known Beetle.
SUPPORTED_BEETLE_USB_IDS = frozenset({("0x1a86", "0x55d4")})


@dataclass(frozen=True)
class UsbPort:
    address: str
    label: str = ""
    vid: str = ""
    pid: str = ""
    serial_number: str = ""

    @property
    def unique_id(self) -> str:
        return self.serial_number.strip().lower()


@dataclass(frozen=True)
class VerifiedRelease:
    version: int
    tag: str
    source_commit: str
    image: bytes
    image_sha256: str

    @property
    def web_url(self) -> str:
        return f"https://github.com/{REPOSITORY}/releases/tag/{self.tag}"


@dataclass(frozen=True)
class FlashTools:
    esptool: Path
    partition_table: bytes
    boot_app0: bytes


def positive_version(value: str) -> int:
    if not re.fullmatch(r"[1-9][0-9]*", value):
        raise argparse.ArgumentTypeError("선행 0 없는 양의 정수여야 합니다.")
    version = int(value)
    if version > 2147483647:
        raise argparse.ArgumentTypeError("버전이 INT_MAX를 초과합니다.")
    return version


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "GitHub의 정확한 TagMachine Beetle 버전 릴리즈를 검증한 뒤 "
            "새 USB 포트가 나타나면 460800bps로 자동 업로드합니다."
        )
    )
    parser.add_argument(
        "--count",
        type=int,
        default=1,
        help="순서대로 업로드할 Beetle 수(기본: 1, Main/Sub는 2)",
    )
    parser.add_argument(
        "--port",
        help="이미 연결된 포트를 명시적으로 사용합니다(--count 1에서만 가능).",
    )
    parser.add_argument(
        "--wait-timeout",
        type=int,
        default=300,
        metavar="SECONDS",
        help="각 보드 연결/분리 대기 시간(기본: 300초)",
    )
    parser.add_argument(
        "--expected-version",
        type=positive_version,
        required=True,
        metavar="N",
        help="다운로드할 고정 검토 태그 HAS1_tagmachine_sub-vN의 버전",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="릴리즈 다운로드와 검증만 하고 USB에는 접근하지 않습니다.",
    )
    parser.add_argument(
        "--arduino-cli",
        default="arduino-cli",
        help="arduino-cli 실행 파일(기본: PATH의 arduino-cli)",
    )
    return parser.parse_args(argv)


def run_json(command: list[str]) -> object:
    result = subprocess.run(
        command, capture_output=True, text=True, check=False, timeout=30
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"명령 실패: {shlex.join(command)}\n{detail}")
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError("arduino-cli JSON 출력을 해석할 수 없습니다.") from error


def normalize_hex(value: object) -> str:
    text = str(value or "").strip().lower()
    if not text:
        return ""
    try:
        return f"0x{int(text, 16):04x}"
    except ValueError:
        return text


def parse_usb_ports(payload: object) -> list[UsbPort]:
    """Accept both current and older arduino-cli board-list JSON layouts."""
    if isinstance(payload, dict) and isinstance(payload.get("detected_ports"), list):
        entries = payload["detected_ports"]
    elif isinstance(payload, list):
        entries = payload
    elif isinstance(payload, dict):
        entries = [payload]
    else:
        return []

    ports: dict[str, UsbPort] = {}
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        raw = entry.get("port", entry)
        if not isinstance(raw, dict):
            continue
        address = str(raw.get("address", "")).strip()
        protocol = str(raw.get("protocol", "serial")).strip().lower()
        label = str(raw.get("label", "")).strip()
        properties = raw.get("properties", {})
        if not isinstance(properties, dict):
            properties = {}
        legacy_boards = entry.get("boards", [])
        legacy_board = (
            legacy_boards[0]
            if isinstance(legacy_boards, list)
            and legacy_boards
            and isinstance(legacy_boards[0], dict)
            else {}
        )
        vid = normalize_hex(
            properties.get("vid")
            or raw.get("vid")
            or entry.get("vid")
            or legacy_board.get("vid")
        )
        pid = normalize_hex(
            properties.get("pid")
            or raw.get("pid")
            or entry.get("pid")
            or legacy_board.get("pid")
        )
        serial_number = str(
            properties.get("serialNumber")
            or properties.get("serial_number")
            or raw.get("serial_number")
            or entry.get("serial_number")
            or ""
        ).strip()
        haystack = f"{address} {label}".lower()
        excluded = any(
            marker in haystack
            for marker in ("bluetooth", "debug-console", "wireless", "network")
        )
        if address and protocol == "serial" and vid and pid and not excluded:
            ports[address] = UsbPort(address, label, vid, pid, serial_number)
    return sorted(ports.values(), key=lambda item: item.address)


def list_usb_ports(cli: str) -> list[UsbPort]:
    return parse_usb_ports(run_json([cli, "board", "list", "--format", "json"]))


def fresh_candidates(
    ports: list[UsbPort], ignored_addresses: set[str], completed_ids: set[str]
) -> list[UsbPort]:
    return [
        port
        for port in ports
        if port.address not in ignored_addresses
        and (not port.unique_id or port.unique_id not in completed_ids)
        and (port.vid, port.pid) in SUPPORTED_BEETLE_USB_IDS
    ]


def unseen_ports(
    ports: list[UsbPort], ignored_addresses: set[str], completed_ids: set[str]
) -> list[UsbPort]:
    return [
        port
        for port in ports
        if port.address not in ignored_addresses
        and (not port.unique_id or port.unique_id not in completed_ids)
    ]


def wait_for_new_port(
    cli: str,
    ignored_addresses: set[str],
    completed_ids: set[str],
    timeout_seconds: int,
) -> UsbPort:
    deadline = time.monotonic() + timeout_seconds
    stable_key = ""
    stable_count = 0
    ignored = set(ignored_addresses)
    while time.monotonic() < deadline:
        ports = list_usb_ports(cli)
        current_addresses = {port.address for port in ports}
        # An initially connected port becomes eligible only after it was
        # physically absent once and then reappears.
        ignored.intersection_update(current_addresses)
        unseen = unseen_ports(ports, ignored, completed_ids)
        unsupported = [
            port
            for port in unseen
            if (port.vid, port.pid) not in SUPPORTED_BEETLE_USB_IDS
        ]
        if unsupported:
            details = "\n".join(
                f"  - {port.address} ({port.vid}/{port.pid})" for port in unsupported
            )
            raise RuntimeError(
                "새 USB 직렬 장치가 확인된 Beetle USB ID가 아니어서 "
                "자동 업로드하지 않았습니다:\n" + details
            )
        candidates = fresh_candidates(ports, ignored, completed_ids)
        if len(candidates) > 1:
            addresses = "\n".join(f"  - {port.address}" for port in candidates)
            raise RuntimeError(
                "새 USB 포트가 여러 개라 자동 선택하지 않았습니다. "
                "하나만 연결하거나 --port를 사용하세요:\n" + addresses
            )
        if len(candidates) == 1:
            candidate = candidates[0]
            key = f"{candidate.address}|{candidate.unique_id}"
            if key == stable_key:
                stable_count += 1
            else:
                stable_key = key
                stable_count = 1
            if stable_count >= STABLE_POLLS:
                return candidate
        else:
            stable_key = ""
            stable_count = 0
        time.sleep(POLL_SECONDS)
    raise RuntimeError(f"{timeout_seconds}초 안에 새 Beetle USB 포트를 찾지 못했습니다.")


def wait_for_physical_disconnect(
    cli: str, uploaded: UsbPort, timeout_seconds: int
) -> set[str]:
    deadline = time.monotonic() + timeout_seconds
    absent_since: float | None = None
    while time.monotonic() < deadline:
        ports = list_usb_ports(cli)
        present = any(
            (uploaded.unique_id and port.unique_id == uploaded.unique_id)
            or (not uploaded.unique_id and port.address == uploaded.address)
            for port in ports
        )
        now = time.monotonic()
        if not present:
            absent_since = absent_since or now
            if now - absent_since >= DISCONNECT_STABLE_SECONDS:
                return {port.address for port in ports}
        else:
            absent_since = None
        time.sleep(POLL_SECONDS)
    raise RuntimeError("업로드한 Beetle의 USB 분리를 확인하지 못했습니다.")


def require_stable_sequence_identity(port: UsbPort, count: int) -> None:
    if count > 1 and not port.unique_id:
        raise RuntimeError(
            "이 USB 포트에는 안정적인 serialNumber가 없어 같은 보드의 재열거를 "
            "다음 보드로 구분할 수 없습니다. 각 보드를 --count 1로 따로 업로드하세요."
        )


def check_core(cli: str) -> None:
    payload = run_json([cli, "core", "list", "--format", "json"])
    platforms = payload.get("platforms", []) if isinstance(payload, dict) else []
    installed = next(
        (
            str(platform.get("installed_version", ""))
            for platform in platforms
            if isinstance(platform, dict) and platform.get("id") == "esp32:esp32"
        ),
        "",
    )
    if installed != CORE_VERSION:
        raise RuntimeError(
            f"ESP32 core {CORE_VERSION}이 필요합니다(현재: {installed or '미설치'}).\n"
            f"  arduino-cli core install esp32:esp32@{CORE_VERSION}"
        )


def load_flash_tools(cli: str) -> FlashTools:
    data_dir = run_json(
        [cli, "config", "get", "directories.data", "--format", "json"]
    )
    if not isinstance(data_dir, str) or not data_dir.strip():
        raise RuntimeError("arduino-cli data 디렉터리를 확인할 수 없습니다.")
    packages = Path(data_dir).expanduser().resolve() / "packages/esp32"
    partitions = (
        packages
        / "hardware"
        / "esp32"
        / CORE_VERSION
        / "tools"
        / "partitions"
    )
    default_partition = partitions / "default.bin"
    boot_app0 = partitions / "boot_app0.bin"
    esptool_dir = packages / "tools" / "esptool_py" / ESPTOOL_VERSION
    esptool = next(
        (path for path in (esptool_dir / "esptool", esptool_dir / "esptool.exe") if path.is_file()),
        None,
    )
    try:
        partition_bytes = default_partition.read_bytes()
        boot_app0_bytes = boot_app0.read_bytes()
    except OSError as error:
        raise RuntimeError("ESP32 core의 default 파티션 자료를 읽을 수 없습니다.") from error
    if (
        len(partition_bytes) != PARTITION_TABLE_SIZE
        or hashlib.sha256(partition_bytes).hexdigest() != DEFAULT_PARTITION_SHA256
        or len(boot_app0_bytes) != OTA_DATA_SIZE
        or hashlib.sha256(boot_app0_bytes).hexdigest() != BOOT_APP0_SHA256
    ):
        raise RuntimeError("설치된 ESP32 core의 baseline 파티션 자료가 고정값과 다릅니다.")
    if esptool is None:
        raise RuntimeError(
            f"ESP32 core용 esptool {ESPTOOL_VERSION}을 찾을 수 없습니다: {esptool_dir}"
        )
    return FlashTools(esptool, partition_bytes, boot_app0_bytes)


def trusted_download_url(url: str) -> bool:
    parsed = urllib.parse.urlsplit(url)
    host = (parsed.hostname or "").lower()
    return parsed.scheme == "https" and (
        host == "github.com"
        or host == "githubusercontent.com"
        or host.endswith(".githubusercontent.com")
    )


class SafeRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, newurl):
        if not trusted_download_url(newurl):
            raise urllib.error.HTTPError(
                newurl, code, "신뢰하지 않는 GitHub 다운로드 redirect", headers, fp
            )
        return super().redirect_request(request, fp, code, msg, headers, newurl)


OPENER = urllib.request.build_opener(SafeRedirect())


def read_url(url: str, limit: int, *, api: bool = False) -> bytes:
    headers = {
        "User-Agent": "New_HAS1-TagMachine-Release-USB-Uploader",
        "Accept": "application/vnd.github+json" if api else "application/octet-stream",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    request = urllib.request.Request(url, headers=headers)
    try:
        with OPENER.open(request, timeout=60) as response:
            final_url = response.geturl()
            final_host = (urllib.parse.urlsplit(final_url).hostname or "").lower()
            if api:
                if final_host != "api.github.com":
                    raise RuntimeError("GitHub API가 예상하지 않은 호스트로 이동했습니다.")
            elif not trusted_download_url(final_url):
                raise RuntimeError("릴리즈 자산이 신뢰하지 않는 호스트로 이동했습니다.")
            length = response.headers.get("Content-Length")
            if length and length.isdigit() and int(length) > limit:
                raise RuntimeError("GitHub 응답이 허용 크기를 초과합니다.")
            data = response.read(limit + 1)
    except urllib.error.HTTPError as error:
        raise RuntimeError(f"GitHub 요청 실패(HTTP {error.code}): {url}") from None
    except urllib.error.URLError as error:
        raise RuntimeError(f"GitHub 요청 실패: {error.reason}") from None
    if len(data) > limit:
        raise RuntimeError("GitHub 응답이 허용 크기를 초과합니다.")
    return data


def api_json(route: str) -> object:
    data = read_url(API_BASE + route, MAX_API_BYTES, api=True)
    try:
        return json.loads(data)
    except json.JSONDecodeError as error:
        raise RuntimeError("GitHub API JSON을 해석할 수 없습니다.") from error


def release_tag(version: int) -> str:
    return f"{DEVICE}-v{version}"


def expected_asset_url(tag: str, name: str) -> str:
    return f"https://github.com/{REPOSITORY}/releases/download/{tag}/{name}"


def validate_release_metadata(payload: object, version: int) -> dict[str, dict]:
    tag = release_tag(version)
    pin = PINNED_RELEASES.get(version)
    if not isinstance(pin, dict) or not isinstance(pin.get("assets"), dict):
        raise RuntimeError(
            f"v{version}은 검토된 USB 릴리즈 allowlist에 없습니다. "
            "업로더의 commit/asset SHA-256 pin을 먼저 갱신하세요."
        )
    if not isinstance(payload, dict):
        raise RuntimeError("GitHub 릴리즈 응답 형식이 잘못되었습니다.")
    if (
        payload.get("tag_name") != tag
        or payload.get("draft") is not False
        or payload.get("prerelease") is not False
        or not isinstance(payload.get("id"), int)
    ):
        raise RuntimeError("요청한 공개 정식 버전 릴리즈가 아닙니다.")
    commit = payload.get("target_commitish")
    if not isinstance(commit, str) or not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise RuntimeError("릴리즈 target commit이 고정된 전체 SHA가 아닙니다.")
    if commit != pin.get("source_commit"):
        raise RuntimeError("릴리즈 target commit이 검토된 고정값과 다릅니다.")
    if payload.get("html_url") != f"https://github.com/{REPOSITORY}/releases/tag/{tag}":
        raise RuntimeError("릴리즈 URL이 예상 저장소/태그와 다릅니다.")

    raw_assets = payload.get("assets")
    if not isinstance(raw_assets, list):
        raise RuntimeError("릴리즈 asset 목록이 없습니다.")
    assets: dict[str, dict] = {}
    for raw in raw_assets:
        if not isinstance(raw, dict) or not isinstance(raw.get("name"), str):
            raise RuntimeError("릴리즈 asset 메타데이터가 잘못되었습니다.")
        name = raw["name"]
        if name in assets:
            raise RuntimeError(f"릴리즈에 중복 asset이 있습니다: {name}")
        assets[name] = raw
    if set(assets) != set(EXPECTED_ASSETS):
        missing = sorted(set(EXPECTED_ASSETS) - set(assets))
        extra = sorted(set(assets) - set(EXPECTED_ASSETS))
        raise RuntimeError(
            "릴리즈 asset 구성이 고정 규격과 다릅니다"
            f"(누락: {', '.join(missing) or '-'}, 추가: {', '.join(extra) or '-'})."
        )

    for name, asset in assets.items():
        size = asset.get("size")
        lower, upper = ASSET_SIZE_LIMITS[name]
        digest = asset.get("digest")
        if (
            asset.get("state") != "uploaded"
            or not isinstance(asset.get("id"), int)
            or not isinstance(size, int)
            or not lower <= size <= upper
            or not isinstance(digest, str)
            or not re.fullmatch(r"sha256:[0-9a-f]{64}", digest)
        ):
            raise RuntimeError(f"릴리즈 asset 메타데이터가 유효하지 않습니다: {name}")
        if digest != f"sha256:{pin['assets'].get(name, '')}":
            raise RuntimeError(
                f"릴리즈 asset SHA-256이 검토된 고정값과 다릅니다: {name}"
            )
        if asset.get("browser_download_url") != expected_asset_url(tag, name):
            raise RuntimeError(f"릴리즈 asset URL이 예상 경로와 다릅니다: {name}")
    return assets


def release_snapshot(payload: object, version: int) -> tuple:
    assets = validate_release_metadata(payload, version)
    assert isinstance(payload, dict)
    return (
        payload["id"],
        payload["tag_name"],
        payload["target_commitish"],
        payload.get("published_at"),
        tuple(
            sorted(
                (
                    asset["id"],
                    name,
                    asset["state"],
                    asset["size"],
                    asset["digest"],
                    asset["browser_download_url"],
                )
                for name, asset in assets.items()
            )
        ),
    )


def resolve_tag_commit(tag: str) -> str:
    quoted = urllib.parse.quote(tag, safe="")
    payload = api_json(f"/git/ref/tags/{quoted}")
    if not isinstance(payload, dict) or not isinstance(payload.get("object"), dict):
        raise RuntimeError("Git tag 참조를 확인할 수 없습니다.")
    obj = payload["object"]
    for _ in range(5):
        kind = obj.get("type")
        sha = obj.get("sha")
        if not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{40}", sha):
            break
        if kind == "commit":
            return sha
        if kind != "tag":
            break
        annotated = api_json(f"/git/tags/{sha}")
        if not isinstance(annotated, dict) or not isinstance(
            annotated.get("object"), dict
        ):
            break
        obj = annotated["object"]
    raise RuntimeError("Git tag가 단일 commit으로 해석되지 않습니다.")


def validate_esp32c3_image(image: bytes) -> None:
    if not 24 <= len(image) <= MAX_IMAGE_BYTES:
        raise RuntimeError("릴리즈 앱 이미지 크기가 OTA 슬롯 안전 범위를 벗어납니다.")
    if image[0] != 0xE9 or not 1 <= image[1] <= 16:
        raise RuntimeError("릴리즈 update.bin이 유효한 ESP 앱 이미지가 아닙니다.")
    chip_id = int.from_bytes(image[12:14], "little")
    flash_size_id = image[3] >> 4
    if chip_id != 5 or flash_size_id != 2:
        raise RuntimeError("릴리즈 update.bin이 ESP32-C3 4MB 이미지가 아닙니다.")


def validate_provenance(data: bytes, version: int, commit: str) -> None:
    try:
        provenance = json.loads(data)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RuntimeError("build-provenance.json을 해석할 수 없습니다.") from error
    expected = {
        "schema": 1,
        "device": DEVICE,
        "firmware_version": version,
        "partition_version": PARTITION_VERSION,
        "partition_scheme": PARTITION_SCHEME,
        "fqbn": RELEASE_FQBN,
        "esp32_core": CORE_VERSION,
        "source_commit": commit,
    }
    if not isinstance(provenance, dict) or any(
        provenance.get(key) != value for key, value in expected.items()
    ):
        raise RuntimeError("릴리즈 build provenance가 요청한 장치/버전과 다릅니다.")
    dependencies = provenance.get("dependencies")
    secureota = dependencies.get("SecureOTA", {}) if isinstance(dependencies, dict) else {}
    if not isinstance(secureota, dict) or (
        secureota.get("commit") != SECUREOTA_REVISION
        or secureota.get("revision") != SECUREOTA_REVISION
    ):
        raise RuntimeError("릴리즈 SecureOTA provenance가 고정 revision과 다릅니다.")
    source_hashes = provenance.get("source_sha256")
    if not isinstance(source_hashes, dict) or not source_hashes:
        raise RuntimeError("릴리즈 source provenance가 비어 있습니다.")
    required_source = f"devices/{DEVICE}/{DEVICE}.ino"
    if required_source not in source_hashes:
        raise RuntimeError("릴리즈 source provenance에 Beetle 스케치가 없습니다.")
    for path, digest in source_hashes.items():
        parts = Path(path).parts if isinstance(path, str) else ()
        if (
            not parts
            or Path(path).is_absolute()
            or ".." in parts
            or not isinstance(digest, str)
            or not re.fullmatch(r"[0-9a-f]{64}", digest)
        ):
            raise RuntimeError("릴리즈 source provenance 항목이 유효하지 않습니다.")


def validate_release_payload(
    payload: object,
    tag_commit: str,
    downloaded: dict[str, bytes],
    version: int,
) -> VerifiedRelease:
    assets = validate_release_metadata(payload, version)
    if set(downloaded) != set(EXPECTED_ASSETS):
        raise RuntimeError("검증할 릴리즈 asset bytes가 완전하지 않습니다.")
    assert isinstance(payload, dict)
    if payload["target_commitish"] != tag_commit:
        raise RuntimeError("릴리즈 target commit과 Git tag commit이 다릅니다.")
    for name, data in downloaded.items():
        if len(data) != assets[name]["size"]:
            raise RuntimeError(f"릴리즈 asset 크기가 API 메타데이터와 다릅니다: {name}")
        actual = hashlib.sha256(data).hexdigest()
        if assets[name]["digest"] != f"sha256:{actual}":
            raise RuntimeError(f"릴리즈 asset SHA-256이 일치하지 않습니다: {name}")

    if downloaded["version.txt"] != str(version).encode("ascii"):
        raise RuntimeError("version.txt가 요청한 버전과 다릅니다.")
    if downloaded["partition_version.txt"] != str(PARTITION_VERSION).encode("ascii"):
        raise RuntimeError("partition_version.txt가 지원 파티션 버전과 다릅니다.")
    update_signature = downloaded["update.sig"]
    if len(update_signature) != 32 or len(downloaded["ota.sig"]) != 32:
        raise RuntimeError("릴리즈 서명 파일 길이가 HMAC-SHA256 규격과 다릅니다.")
    canonical_ota = (
        f"IGOTA1|{DEVICE}|{version}|{PARTITION_VERSION}|{PARTITION_SCHEME}|"
        f"{update_signature.hex()}\n"
    ).encode("ascii")
    if downloaded["ota.txt"] != canonical_ota:
        raise RuntimeError("ota.txt와 update.sig가 canonical 릴리즈 규격과 다릅니다.")

    image = downloaded["update.bin"]
    validate_esp32c3_image(image)
    validate_provenance(downloaded["build-provenance.json"], version, tag_commit)
    return VerifiedRelease(
        version=version,
        tag=release_tag(version),
        source_commit=tag_commit,
        image=image,
        image_sha256=hashlib.sha256(image).hexdigest(),
    )


def download_pinned_release(version: int) -> VerifiedRelease:
    tag = release_tag(version)
    route = "/releases/tags/" + urllib.parse.quote(tag, safe="")
    before = api_json(route)
    snapshot = release_snapshot(before, version)
    before_commit = resolve_tag_commit(tag)
    assert isinstance(before, dict)
    if before["target_commitish"] != before_commit:
        raise RuntimeError("릴리즈와 Git tag가 같은 commit을 가리키지 않습니다.")

    metadata = validate_release_metadata(before, version)
    downloaded: dict[str, bytes] = {}
    for name in EXPECTED_ASSETS:
        size = metadata[name]["size"]
        downloaded[name] = read_url(metadata[name]["browser_download_url"], size)

    # Detect a tag move or asset replacement that raced with this download.
    after = api_json(route)
    after_commit = resolve_tag_commit(tag)
    if release_snapshot(after, version) != snapshot or after_commit != before_commit:
        raise RuntimeError("다운로드 도중 릴리즈 또는 Git tag가 변경되었습니다.")
    return validate_release_payload(after, after_commit, downloaded, version)


def upload_command(cli: str, port: str, image: Path) -> list[str]:
    # The esp32 core's `esptool` programmer recipe writes only the app at
    # 0x10000. The normal upload recipe also writes bootloader/partition data.
    return [
        cli,
        "upload",
        "--fqbn",
        UPLOAD_FQBN,
        "--port",
        port,
        "--input-file",
        str(image),
        "--programmer",
        "esptool",
        "--verify",
    ]


def materialize_image(release: VerifiedRelease, directory: Path) -> Path:
    directory.mkdir()
    image = directory / "update.bin"
    image.write_bytes(release.image)
    if hashlib.sha256(image.read_bytes()).hexdigest() != release.image_sha256:
        raise RuntimeError("임시 업로드 이미지 SHA-256 검증에 실패했습니다.")
    return image


def read_flash_command(
    tools: FlashTools, port: str, address: int, size: int, output: Path
) -> list[str]:
    return [
        str(tools.esptool),
        "--chip",
        "esp32c3",
        "--port",
        port,
        "--baud",
        "460800",
        "--before",
        "default-reset",
        "--after",
        "hard-reset",
        "read-flash",
        "--no-progress",
        hex(address),
        hex(size),
        str(output),
    ]


def validate_device_baseline(
    partition_table: bytes, ota_data: bytes, tools: FlashTools
) -> None:
    if partition_table != tools.partition_table:
        raise RuntimeError(
            "보드의 파티션 테이블이 ESP32 core 3.3.11 default와 다릅니다. "
            "앱 영역만 덮어쓰면 안전하지 않아 중단했습니다."
        )
    if ota_data != tools.boot_app0:
        raise RuntimeError(
            "보드가 초기 app0 선택 상태가 아닙니다(이미 OTA 슬롯이 바뀌었을 수 있음). "
            "이 스크립트는 v3 USB baseline 장치에만 사용하세요."
        )


def verify_device_baseline(tools: FlashTools, port: str, directory: Path) -> None:
    partition_output = directory / "device-partitions.bin"
    ota_output = directory / "device-otadata.bin"
    reads = (
        (
            PARTITION_TABLE_OFFSET,
            PARTITION_TABLE_SIZE,
            partition_output,
        ),
        (OTA_DATA_OFFSET, OTA_DATA_SIZE, ota_output),
    )
    print("🔎 보드의 default 파티션/app0 baseline 확인 중...")
    for address, size, output in reads:
        result = subprocess.run(
            read_flash_command(tools, port, address, size, output),
            check=False,
        )
        if result.returncode != 0:
            raise RuntimeError(
                "보드 baseline 읽기에 실패했습니다. 같은 연결에서 자동 재시도하지 않습니다."
            )
    try:
        partition_table = partition_output.read_bytes()
        ota_data = ota_output.read_bytes()
    except OSError as error:
        raise RuntimeError("보드 baseline 읽기 결과를 확인할 수 없습니다.") from error
    validate_device_baseline(partition_table, ota_data, tools)
    print("✅ default 파티션 및 app0 선택 상태 확인")


def port_holder(address: str) -> str:
    lsof = shutil.which("lsof")
    if not lsof:
        return ""
    try:
        result = subprocess.run(
            [lsof, address], capture_output=True, text=True, timeout=5, check=False
        )
    except (OSError, subprocess.SubprocessError):
        return ""
    lines = [line for line in result.stdout.splitlines()[1:] if line.strip()]
    return lines[0] if lines else ""


def explicit_port(cli: str, address: str) -> UsbPort:
    for port in list_usb_ports(cli):
        if port.address == address:
            if (port.vid, port.pid) not in SUPPORTED_BEETLE_USB_IDS:
                raise RuntimeError(
                    f"지정한 포트가 확인된 Beetle USB ID가 아닙니다: {address} "
                    f"({port.vid}/{port.pid})"
                )
            return port
    if not address.upper().startswith("COM") and not Path(address).exists():
        raise RuntimeError(f"지정한 포트를 찾을 수 없습니다: {address}")
    return UsbPort(address=address, label="explicit port")


def main() -> int:
    args = parse_args()
    if args.count <= 0:
        raise RuntimeError("--count는 1 이상이어야 합니다.")
    if args.wait_timeout <= 0:
        raise RuntimeError("--wait-timeout은 1 이상이어야 합니다.")
    if args.port and args.count != 1:
        raise RuntimeError("--port는 --count 1에서만 사용할 수 있습니다.")
    if shutil.which(args.arduino_cli) is None and not Path(args.arduino_cli).is_file():
        raise RuntimeError("arduino-cli를 찾을 수 없습니다.")

    # Snapshot before network work, so a Beetle connected during the download
    # is still considered newly attached. Dry-run never inspects USB.
    initial_ports = [] if args.dry_run or args.port else list_usb_ports(args.arduino_cli)
    ignored_addresses = {port.address for port in initial_ports}
    if ignored_addresses:
        print("ℹ️  시작할 때 이미 있던 USB 포트는 자동 선택하지 않습니다:")
        for address in sorted(ignored_addresses):
            print(f"   - {address}")

    check_core(args.arduino_cli)
    flash_tools = load_flash_tools(args.arduino_cli)
    tag = release_tag(args.expected_version)
    print(f"⬇️  GitHub 버전 릴리즈 검증 중: {tag}")
    release = download_pinned_release(args.expected_version)
    print(f"✅ Release: {release.web_url}")
    print(f"   commit: {release.source_commit}")
    print(
        f"   update.bin: {len(release.image):,}B, SHA-256 {release.image_sha256}"
    )
    print("   upload: app@0x10000 only, 460800bps")

    if args.dry_run:
        print("✅ dry-run 완료. USB 조회/업로드는 수행하지 않았습니다.")
        return 0

    completed_ids: set[str] = set()
    with tempfile.TemporaryDirectory(prefix="tagmachine-beetle-release-") as work:
        work_path = Path(work)
        for index in range(args.count):
            if args.port:
                port = explicit_port(args.arduino_cli, args.port)
            else:
                print(
                    f"🔌 Beetle {index + 1}/{args.count} 한 대만 USB에 연결하세요 "
                    f"(최대 {args.wait_timeout}초)."
                )
                port = wait_for_new_port(
                    args.arduino_cli,
                    ignored_addresses,
                    completed_ids,
                    args.wait_timeout,
                )
            require_stable_sequence_identity(port, args.count)
            holder = port_holder(port.address)
            if holder:
                raise RuntimeError(
                    f"포트를 다른 프로그램이 사용 중입니다: {port.address}\n  {holder}"
                )

            # Each board gets a fresh directory, so esp32 core 3.3.11 cannot
            # reuse a *_flashed.bin differential-flash reference from another
            # board. The bytes are checked again immediately before upload.
            board_dir = work_path / f"board-{index + 1}"
            image = materialize_image(release, board_dir)
            verify_device_baseline(flash_tools, port.address, board_dir)
            print(
                f"⚡ Beetle {index + 1}/{args.count} 릴리즈 v{release.version} 업로드: "
                f"{port.address} ({port.vid or '?'}/{port.pid or '?'})"
            )
            result = subprocess.run(
                upload_command(args.arduino_cli, port.address, image),
                check=False,
            )
            if result.returncode != 0:
                raise RuntimeError(
                    "업로드에 실패했습니다. 같은 연결에서 자동 재시도하지 않습니다. "
                    "BOOT를 누른 채 RST를 눌렀다 놓고, BOOT를 놓은 뒤 다시 실행하세요."
                )
            print(f"✅ Beetle {index + 1}/{args.count} 업로드 완료: {port.address}")
            if port.unique_id:
                completed_ids.add(port.unique_id)
            if index + 1 < args.count:
                print("🔌 방금 업로드한 Beetle을 USB에서 완전히 분리하세요.")
                ignored_addresses = wait_for_physical_disconnect(
                    args.arduino_cli, port, args.wait_timeout
                )
                print("✅ 분리 확인. 다음 Beetle을 연결할 수 있습니다.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n❌ 사용자 취소")
        sys.exit(130)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"❌ {error}")
        sys.exit(1)
