#!/usr/bin/env python3
"""Build once, then upload TagMachine Beetles as they are connected by USB.

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


SKETCH_DIR = Path(__file__).resolve().parents[1]
ROOT = Path(__file__).resolve().parents[3]
PREPARE_LIBRARIES = ROOT / "devices/iotglove/tools/prepare_libraries.py"
CORE_VERSION = "3.3.11"
SECUREOTA_REVISION = "162db758e6806895ef10ca39b101f9f8753bdc20"
FQBN = (
    "esp32:esp32:dfrobot_beetle_esp32c3:UploadSpeed=460800,"
    "CDCOnBoot=cdc,CPUFreq=160,FlashFreq=80,FlashMode=qio,FlashSize=4M,"
    "PartitionScheme=default,DebugLevel=none,EraseFlash=none"
)
SLOT_SIZE = 0x140000
REQUIRED_SLOT_MARGIN = 32 * 1024
POLL_SECONDS = 0.5
STABLE_POLLS = 3
DISCONNECT_STABLE_SECONDS = 2.0
SOURCE_SUFFIXES = {".ino", ".h", ".hpp", ".c", ".cpp", ".S", ".s"}
REGISTRY_LIBRARIES = (
    "Adafruit PN532@1.3.4",
    "Adafruit BusIO@1.17.4",
    "ArduinoJson@7.4.3",
)
REGISTRY_LIBRARY_VERSIONS = {
    "Adafruit_PN532": "1.3.4",
    "Adafruit_BusIO": "1.17.4",
    "ArduinoJson": "7.4.3",
}
# DFRobot's Beetle ESP32-C3 board exposes its CH343 USB-UART bridge with this
# generic WCH identity. It is not globally unique to Beetle hardware, so auto
# mode still requires the operator to connect exactly one known Beetle.
SUPPORTED_BEETLE_USB_IDS = frozenset({("0x1a86", "0x55d4")})
REQUIRED_LIBRARY_DIRS = (
    "Adafruit_PN532",
    "Adafruit_BusIO",
    "ArduinoJson",
    "HAS2_Wifi",
    "SecureOTA",
    "SimpleTimer",
)
CUSTOM_LIBRARY_DIRS = ("HAS2_Wifi", "SecureOTA", "SimpleTimer")


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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "TagMachine Beetle 펌웨어를 한 번 빌드한 뒤 새 USB 포트가 나타나면 "
            "460800bps로 자동 업로드합니다."
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
        "--secret-file",
        type=Path,
        help="실제 HMAC secrets.h 경로(기본: 스케치 폴더의 secrets.h)",
    )
    parser.add_argument(
        "--expected-version",
        type=int,
        help="소스 FIRMWARE_VER가 이 값과 다르면 빌드 전에 중단합니다.",
    )
    parser.add_argument(
        "--libraries-dir",
        type=Path,
        help="이미 준비된 라이브러리 모음. 생략하면 build 캐시를 준비합니다.",
    )
    parser.add_argument(
        "--refresh-dependencies",
        action="store_true",
        help="자동 준비 라이브러리 캐시를 지우고 다시 받습니다.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="실제 키로 클린 빌드와 슬롯 검증만 하고 업로드하지 않습니다.",
    )
    parser.add_argument(
        "--arduino-cli",
        default="arduino-cli",
        help="arduino-cli 실행 파일(기본: PATH의 arduino-cli)",
    )
    return parser.parse_args()


def command_prefix(cli: str, config: Path | None) -> list[str]:
    result = [cli]
    if config is not None:
        result += ["--config-file", str(config)]
    return result


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
        # A VID/PID pair is required because names such as COM3 or cu.usbserial
        # are not a board identity. DFRobot's USB-UART has no unique board name.
        if address and protocol == "serial" and vid and pid and not excluded:
            ports[address] = UsbPort(address, label, vid, pid, serial_number)
    return sorted(ports.values(), key=lambda item: item.address)


def list_usb_ports(cli: str, config: Path | None) -> list[UsbPort]:
    payload = run_json(
        command_prefix(cli, config) + ["board", "list", "--format", "json"]
    )
    return parse_usb_ports(payload)


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
    config: Path | None,
    ignored_addresses: set[str],
    completed_ids: set[str],
    timeout_seconds: int,
) -> UsbPort:
    deadline = time.monotonic() + timeout_seconds
    stable_key = ""
    stable_count = 0
    ignored = set(ignored_addresses)
    while time.monotonic() < deadline:
        ports = list_usb_ports(cli, config)
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
                f"  - {port.address} ({port.vid}/{port.pid})"
                for port in unsupported
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
    cli: str, config: Path | None, uploaded: UsbPort, timeout_seconds: int
) -> set[str]:
    deadline = time.monotonic() + timeout_seconds
    absent_since: float | None = None
    while time.monotonic() < deadline:
        ports = list_usb_ports(cli, config)
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


def read_firmware_version(sketch: Path) -> int:
    match = re.search(
        r"^\s*#define\s+FIRMWARE_VER\s+(\d+)\s*$",
        sketch.read_text(encoding="utf-8"),
        re.MULTILINE,
    )
    if not match or int(match.group(1)) <= 0:
        raise RuntimeError(f"FIRMWARE_VER를 읽을 수 없습니다: {sketch}")
    return int(match.group(1))


def require_stable_sequence_identity(port: UsbPort, count: int) -> None:
    if count > 1 and not port.unique_id:
        raise RuntimeError(
            "이 USB 포트에는 안정적인 serialNumber가 없어 같은 보드의 재열거를 "
            "다음 보드로 구분할 수 없습니다. 각 보드를 --count 1로 따로 업로드하세요."
        )


def validated_secret_header(path: Path) -> str:
    try:
        contents = path.read_text(encoding="utf-8")
    except OSError as error:
        raise RuntimeError(
            f"실제 OTA 키가 든 secrets.h가 필요합니다: {path}"
        ) from error
    match = re.search(
        r'^\s*#define\s+HMAC_SECRET\s+("(?:\\.|[^"\\])*")\s*$',
        contents,
        re.MULTILINE,
    )
    if not match:
        raise RuntimeError(f"HMAC_SECRET 문자열 매크로를 읽을 수 없습니다: {path}")
    try:
        secret = json.loads(match.group(1))
    except json.JSONDecodeError as error:
        raise RuntimeError("secrets.h의 HMAC_SECRET 문자열이 잘못되었습니다.") from error
    normalized = secret.strip() if isinstance(secret, str) else ""
    disabled = (
        not normalized
        or normalized in {
            "CHANGE_THIS_TO_YOUR_SECRET",
            "REPLACE_WITH_DEPLOYMENT_SECRET",
            "__COMPILE_ONLY_DO_NOT_DEPLOY__",
            "TAGMACHINE_CI_LINK_VALIDATION_PUBLIC_KEY_NEVER_RELEASE",
        }
        or "COMPILE_ONLY" in normalized
        or "PLACEHOLDER" in normalized
        or normalized.startswith("REPLACE_WITH_")
        or any(ord(character) < 32 for character in secret)
    )
    if disabled:
        raise RuntimeError("실제 배포 HMAC_SECRET이 아니므로 USB 빌드를 중단했습니다.")
    return contents


def stage_sketch(destination: Path, secret_header: str) -> Path:
    staged = destination / SKETCH_DIR.name
    staged.mkdir(parents=True)
    for source in SKETCH_DIR.iterdir():
        if (
            source.is_file()
            and source.suffix in SOURCE_SUFFIXES
            and source.name != "secrets.h"
        ):
            shutil.copy2(source, staged / source.name)
    (staged / "secrets.h").write_text(secret_header, encoding="utf-8")
    return staged


def check_core(cli: str, config: Path | None) -> None:
    payload = run_json(
        command_prefix(cli, config) + ["core", "list", "--format", "json"]
    )
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


def directory_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    for item in sorted(
        (entry for entry in path.rglob("*") if entry.is_file()),
        key=lambda entry: entry.relative_to(path).as_posix(),
    ):
        relative = item.relative_to(path).as_posix().encode("utf-8")
        digest.update(len(relative).to_bytes(4, "big"))
        digest.update(relative)
        digest.update(hashlib.sha256(item.read_bytes()).digest())
    return digest.hexdigest()


def library_version(path: Path) -> str:
    properties = path / "library.properties"
    try:
        contents = properties.read_text(encoding="utf-8")
    except OSError:
        return ""
    match = re.search(r"^version\s*=\s*(\S+)\s*$", contents, re.MULTILINE)
    return match.group(1) if match else ""


def validate_dependency_provenance(path: Path) -> None:
    provenance_path = path / "iotglove-dependencies.json"
    try:
        provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(
            "의존성 provenance가 없거나 손상되었습니다. "
            "--refresh-dependencies로 다시 준비하세요."
        ) from error
    patch_path = PREPARE_LIBRARIES.with_name("has2-wifi-result-api.patch")
    patch_hash = hashlib.sha256(patch_path.read_bytes()).hexdigest()
    secureota = provenance.get("SecureOTA", {})
    has2_wifi = provenance.get("HAS2_Wifi", {})
    if (
        secureota.get("commit") != SECUREOTA_REVISION
        or secureota.get("revision") != SECUREOTA_REVISION
        or has2_wifi.get("branch") != "first_store"
        or has2_wifi.get("patch_sha256") != patch_hash
    ):
        raise RuntimeError(
            "의존성 provenance가 현재 고정 버전과 다릅니다. "
            "--refresh-dependencies로 다시 준비하세요."
        )
    for name in CUSTOM_LIBRARY_DIRS:
        expected = provenance.get(name, {}).get("tree_sha256", "")
        actual = directory_sha256(path / name)
        if not expected or expected != actual:
            raise RuntimeError(
                f"{name} 라이브러리 내용이 provenance와 다릅니다. "
                "--refresh-dependencies로 다시 준비하세요."
            )


def validate_library_collection(path: Path) -> Path:
    missing = [name for name in REQUIRED_LIBRARY_DIRS if not (path / name).is_dir()]
    if missing:
        raise RuntimeError(
            f"라이브러리 디렉터리가 불완전합니다: {path}\n누락: {', '.join(missing)}"
        )
    wrong_versions = [
        f"{name}={library_version(path / name) or '미확인'}"
        for name, expected in REGISTRY_LIBRARY_VERSIONS.items()
        if library_version(path / name) != expected
    ]
    if wrong_versions:
        raise RuntimeError(
            "라이브러리 버전이 고정값과 다릅니다: " + ", ".join(wrong_versions)
        )
    validate_dependency_provenance(path)
    return path


def prepare_cached_libraries(cli: str, refresh: bool) -> tuple[Path, Path]:
    cache_root = ROOT / "build/tagmachine-beetle-usb"
    user_dir = cache_root / "arduino-user"
    libraries = user_dir / "libraries"
    config = cache_root / "arduino-cli.yaml"
    if refresh and user_dir.exists():
        shutil.rmtree(user_dir)
    user_dir.mkdir(parents=True, exist_ok=True)
    config.write_text(
        "directories:\n  user: " + json.dumps(str(user_dir)) + "\n",
        encoding="utf-8",
    )
    prefix = command_prefix(cli, config)
    subprocess.run(prefix + ["lib", "install", *REGISTRY_LIBRARIES], check=True)

    custom_present = [(libraries / name).is_dir() for name in CUSTOM_LIBRARY_DIRS]
    provenance = libraries / "iotglove-dependencies.json"
    if not all(custom_present) or not provenance.is_file():
        if any(custom_present):
            raise RuntimeError(
                "의존성 캐시가 불완전합니다. --refresh-dependencies로 다시 준비하세요."
            )
        subprocess.run(
            [
                sys.executable,
                str(PREPARE_LIBRARIES),
                "--libraries-dir",
                str(libraries),
            ],
            check=True,
        )
    return validate_library_collection(libraries), config


def compile_firmware(
    cli: str,
    config: Path | None,
    staged_sketch: Path,
    libraries: Path,
    build_path: Path,
    output_path: Path,
) -> Path:
    command = command_prefix(cli, config) + [
        "compile",
        "--clean",
        "--fqbn",
        FQBN,
        "--libraries",
        str(libraries),
        "--library",
        str(ROOT / "libraries/TagMachineProtocol"),
        "--library",
        str(ROOT / "libraries/IoTGloveProtocol"),
        "--build-path",
        str(build_path),
        "--output-dir",
        str(output_path),
        str(staged_sketch),
    ]
    print("🔨 Beetle 펌웨어 클린 빌드 중...")
    subprocess.run(command, check=True)
    images = list(output_path.glob("*.ino.bin"))
    if len(images) != 1:
        raise RuntimeError("업로드할 앱 바이너리를 정확히 하나 찾지 못했습니다.")
    image = images[0]
    size = image.stat().st_size
    if size > SLOT_SIZE - REQUIRED_SLOT_MARGIN:
        raise RuntimeError(
            f"앱 이미지 {size:,}B가 OTA 슬롯 32KiB 안전 기준을 초과합니다."
        )
    digest = hashlib.sha256(image.read_bytes()).hexdigest()
    print(
        f"✅ 빌드 완료: {size:,}B, 슬롯 여유 {SLOT_SIZE - size:,}B, "
        f"SHA-256 {digest}"
    )
    return image


def upload_command(
    cli: str, config: Path | None, port: str, output_path: Path
) -> list[str]:
    return command_prefix(cli, config) + [
        "upload",
        "--fqbn",
        FQBN,
        "--port",
        port,
        "--input-dir",
        str(output_path),
        "--upload-property",
        "upload.extra_flags=--no-fast-flash",
        "--verify",
    ]


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


def explicit_port(cli: str, config: Path | None, address: str) -> UsbPort:
    for port in list_usb_ports(cli, config):
        if port.address == address:
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
    if not args.dry_run and args.expected_version is None:
        raise RuntimeError(
            "실제 업로드에는 --expected-version을 반드시 지정해야 합니다."
        )
    if args.refresh_dependencies and args.libraries_dir:
        raise RuntimeError("--refresh-dependencies와 --libraries-dir는 함께 쓸 수 없습니다.")
    if shutil.which(args.arduino_cli) is None and not Path(args.arduino_cli).is_file():
        raise RuntimeError("arduino-cli를 찾을 수 없습니다.")

    secret_path = (
        args.secret_file or (SKETCH_DIR / "secrets.h")
    ).expanduser().resolve()
    secret_header = validated_secret_header(secret_path)
    version = read_firmware_version(SKETCH_DIR / "HAS1_tagmachine_sub.ino")
    if args.expected_version is not None and version != args.expected_version:
        raise RuntimeError(
            f"펌웨어 버전이 다릅니다: 기대 v{args.expected_version}, 소스 v{version}"
        )
    print(f"📌 업로드 대상 펌웨어: v{version}, 속도 460800bps")

    # Snapshot before dependency preparation/build, so a Beetle connected while
    # compilation runs is still considered newly attached.
    initial_config: Path | None = None
    initial_ports = (
        []
        if args.dry_run or args.port
        else list_usb_ports(args.arduino_cli, initial_config)
    )
    ignored_addresses = {port.address for port in initial_ports}
    if ignored_addresses:
        print("ℹ️  시작할 때 이미 있던 USB 포트는 자동 선택하지 않습니다:")
        for address in sorted(ignored_addresses):
            print(f"   - {address}")

    if args.libraries_dir:
        libraries = validate_library_collection(
            args.libraries_dir.expanduser().resolve()
        )
        config = None
    else:
        libraries, config = prepare_cached_libraries(
            args.arduino_cli, args.refresh_dependencies
        )
    check_core(args.arduino_cli, config)

    with tempfile.TemporaryDirectory(prefix="tagmachine-beetle-usb-") as work:
        work_path = Path(work)
        staged = stage_sketch(work_path / "stage", secret_header)
        output_path = work_path / "output"
        build_path = work_path / "build"
        output_path.mkdir()
        build_path.mkdir()
        compile_firmware(
            args.arduino_cli, config, staged, libraries, build_path, output_path
        )

        if args.dry_run:
            print("✅ dry-run 완료. 실제 USB 업로드는 수행하지 않았습니다.")
            return 0

        completed_ids: set[str] = set()
        for index in range(args.count):
            if args.port:
                port = explicit_port(args.arduino_cli, config, args.port)
            else:
                print(
                    f"🔌 Beetle {index + 1}/{args.count} 한 대만 USB에 연결하세요 "
                    f"(최대 {args.wait_timeout}초)."
                )
                port = wait_for_new_port(
                    args.arduino_cli,
                    config,
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
            print(
                f"⚡ Beetle {index + 1}/{args.count} 업로드: {port.address} "
                f"({port.vid or '?'}/{port.pid or '?'})"
            )
            result = subprocess.run(
                upload_command(args.arduino_cli, config, port.address, output_path),
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
                    args.arduino_cli, config, port, args.wait_timeout
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
