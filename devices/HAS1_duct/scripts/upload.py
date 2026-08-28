"""검증된 덕트 빌드 결과를 TTGO T1에 USB로 업로드한다.

사용법:
    python3 scripts/upload.py
    python3 scripts/upload.py --port /dev/cu.SLAB_USBtoUART
    python3 scripts/upload.py --dry-run

이 스크립트는 빌드하지 않는다. ``build/esp32.esp32.ttgo-t1``에 있는
부트로더, 파티션 테이블, boot_app0, 앱 바이너리를 한 번에 플래싱한다.
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time

sys.stdout.reconfigure(encoding="utf-8")

# TTGO T1의 기본 921600은 일부 USB 시리얼 어댑터에서 stub flasher 전환 후
# 연결이 끊길 수 있다. updated_itembox에서 검증된 460800을 사용한다.
FQBN = "esp32:esp32:ttgo-t1:UploadSpeed=460800"
BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(BASE_DIR, "build", "esp32.esp32.ttgo-t1")
ARDUINO_SKETCH_CACHE = os.path.join(
    os.path.expanduser("~"), "Library", "Caches", "arduino", "sketches"
)
APP_BIN_NAME = "HAS1_duct.ino.bin"
BUILD_OPTIONS_NAME = "build.options.json"
ELF_FILE_NAME = "HAS1_duct.ino.elf"
MAP_FILE_NAME = "HAS1_duct.ino.map"
PLACEHOLDER_SECRET = "CHANGE_THIS_TO_YOUR_SECRET"
THREE_ARG_SITUATION_SYMBOL = "_ZN9HAS2_Wifi9SituationE6StringS0_S0_"
SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hpp")

REQUIRED_BUILD_FILES = (
    APP_BIN_NAME,
    BUILD_OPTIONS_NAME,
    ELF_FILE_NAME,
    "HAS1_duct.ino.bootloader.bin",
    "HAS1_duct.ino.partitions.bin",
    "boot_app0.bin",
    "flash_args",
    MAP_FILE_NAME,
)

# TTGO T1은 주로 CP2102 또는 CH340 계열 USB 시리얼로 인식된다.
PORT_PATTERNS = (
    "/dev/cu.usbserial*",
    "/dev/cu.SLAB_USBtoUART*",
    "/dev/cu.wchusbserial*",
)
DEFAULT_WAIT_TIMEOUT_SEC = 120


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="검증된 HAS1_duct 빌드를 TTGO T1에 USB로 업로드합니다."
    )
    parser.add_argument(
        "--port",
        help="업로드할 시리얼 포트. 생략하면 연결된 TTGO T1 포트를 자동 인식합니다.",
    )
    parser.add_argument(
        "--wait-timeout",
        type=int,
        default=DEFAULT_WAIT_TIMEOUT_SEC,
        metavar="SECONDS",
        help=f"보드 연결 대기 시간(기본: {DEFAULT_WAIT_TIMEOUT_SEC}초)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="빌드와 포트만 검사하고 실제 플래시는 실행하지 않습니다.",
    )
    parser.add_argument(
        "--yes",
        action="store_true",
        help="대상 보드 확인 문구를 생략합니다. 자동화된 환경에서만 사용하세요.",
    )
    return parser.parse_args()


def find_ports() -> list[str]:
    ports: set[str] = set()
    for pattern in PORT_PATTERNS:
        ports.update(glob.glob(pattern))
    return sorted(ports)


def wait_for_ports(timeout_sec: int) -> list[str]:
    ports = find_ports()
    if ports:
        return ports

    if timeout_sec <= 0:
        return []

    print(f"🔌 TTGO T1을 USB에 연결해 주세요... (최대 {timeout_sec}초 대기)")
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        ports = find_ports()
        if ports:
            time.sleep(1)  # 포트 노드가 안정화될 때까지 대기
            stable_ports = find_ports()
            if stable_ports:
                return stable_ports
        time.sleep(0.5)
    return []


def select_port(requested_port: str | None, timeout_sec: int) -> str | None:
    if requested_port:
        if not os.path.exists(requested_port):
            print(f"❌ 지정한 포트를 찾을 수 없습니다: {requested_port}")
            return None
        return requested_port

    ports = wait_for_ports(timeout_sec)
    if not ports:
        print("❌ 보드를 찾지 못했습니다. USB 케이블과 CP2102/CH340 드라이버를 확인하세요.")
        return None
    if len(ports) > 1:
        print("❌ 업로드 가능한 포트가 여러 개라 자동 선택하지 않았습니다:")
        for port in ports:
            print(f"   - {port}")
        print("   사용할 포트를 --port 옵션으로 지정하세요.")
        return None
    return ports[0]


def port_holder(port: str) -> str | None:
    """포트를 점유 중인 첫 프로세스 정보를 반환한다."""
    if shutil.which("lsof") is None:
        return None
    try:
        result = subprocess.run(
            ["lsof", port], capture_output=True, text=True, timeout=5, check=False
        )
    except (OSError, subprocess.SubprocessError):
        return None
    lines = [line for line in result.stdout.splitlines()[1:] if line.strip()]
    return lines[0] if lines else None


def source_files() -> list[str]:
    files: list[str] = []
    for pattern in ("*.ino", "*.h"):
        files.extend(glob.glob(os.path.join(BASE_DIR, pattern)))
    return files


def compiled_source_files(elf_path: str) -> list[str]:
    """ELF 디버그 문자열에서 실제 컴파일된 외부 소스를 찾는다."""
    strings_command = shutil.which("strings")
    if strings_command is None:
        return []
    try:
        result = subprocess.run(
            [strings_command, elf_path],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return []
    if result.returncode != 0:
        return []

    compiled_sources = {
        line.strip()
        for line in result.stdout.splitlines()
        if os.path.isabs(line.strip())
        and line.strip().lower().endswith(SOURCE_SUFFIXES)
        and not line.strip().startswith(ARDUINO_SKETCH_CACHE + os.sep)
        and os.path.isfile(line.strip())
    }
    return sorted(compiled_sources)


def dependency_files_from_map(map_path: str) -> tuple[list[str], list[str]]:
    """링커 맵의 모든 로컬 객체에 대응하는 dependency 파일을 찾는다."""
    try:
        with open(map_path, encoding="utf-8", errors="ignore") as map_file:
            object_paths = set(
                re.findall(
                    re.escape(ARDUINO_SKETCH_CACHE) + r"/[^\s()]+\.o",
                    map_file.read(),
                )
            )
    except OSError:
        return [], []
    dependency_paths = []
    missing_paths = []
    for object_path in object_paths:
        dependency_path = object_path[:-2] + ".d"
        if os.path.isfile(dependency_path):
            dependency_paths.append(dependency_path)
        else:
            missing_paths.append(dependency_path)
    return sorted(dependency_paths), sorted(missing_paths)


def read_make_dependencies(path: str) -> list[str]:
    """GCC가 생성한 Make dependency 파일에서 존재하는 입력 파일을 읽는다."""
    try:
        with open(path, encoding="utf-8", errors="ignore") as dependency_file:
            content = dependency_file.read().replace("\\\n", " ")
    except OSError:
        return []
    _, separator, dependency_text = content.partition(":")
    if not separator:
        return []
    try:
        candidates = shlex.split(dependency_text)
    except ValueError:
        return []
    return [path for path in candidates if os.path.isabs(path)]


def read_firmware_version() -> str | None:
    sketch_path = os.path.join(BASE_DIR, "HAS1_duct.ino")
    try:
        with open(sketch_path, encoding="utf-8") as sketch:
            match = re.search(
                r"^\s*#define\s+FIRMWARE_VER\s+(\d+)\s*$",
                sketch.read(),
                re.MULTILINE,
            )
    except OSError:
        return None
    return match.group(1) if match else None


def read_hmac_secret(path: str) -> str | None:
    try:
        with open(path, encoding="utf-8") as secret_file:
            match = re.search(
                r'\bHMAC_SECRET\b\s*(?:=)?\s*"([^"]+)"', secret_file.read()
            )
    except OSError:
        return None
    return match.group(1) if match else None


def validate_build() -> str | None:
    if shutil.which("arduino-cli") is None:
        print("❌ arduino-cli를 찾을 수 없습니다. 먼저 Arduino CLI를 설치하세요.")
        return None

    missing = [
        os.path.join(BUILD_DIR, filename)
        for filename in REQUIRED_BUILD_FILES
        if not os.path.isfile(os.path.join(BUILD_DIR, filename))
    ]
    if missing:
        print("❌ 전체 플래시에 필요한 빌드 결과물이 없습니다:")
        for path in missing:
            print(f"   - {path}")
        print("   검증된 HAS2_Wifi 3인자 API로 먼저 전체 빌드하세요.")
        return None

    app_bin = os.path.join(BUILD_DIR, APP_BIN_NAME)
    elf_path = os.path.join(BUILD_DIR, ELF_FILE_NAME)
    map_path = os.path.join(BUILD_DIR, MAP_FILE_NAME)
    build_options_path = os.path.join(BUILD_DIR, BUILD_OPTIONS_NAME)
    try:
        with open(build_options_path, encoding="utf-8") as build_options_file:
            build_fqbn = str(json.load(build_options_file).get("fqbn", ""))
    except (OSError, ValueError, TypeError):
        build_fqbn = ""
    if not build_fqbn.startswith("esp32:esp32:ttgo-t1"):
        print("❌ 빌드 대상이 ESP32 TTGO T1인지 확인할 수 없습니다.")
        print(f"   build FQBN: {build_fqbn or '알 수 없음'}")
        return None

    newest_source = max(source_files(), key=os.path.getmtime, default=None)
    if newest_source and os.path.getmtime(app_bin) < os.path.getmtime(newest_source):
        print("❌ 앱 바이너리가 소스보다 오래되어 업로드를 중단했습니다.")
        print(f"   바이너리: {app_bin}")
        print(f"   더 최신인 소스: {newest_source}")
        print("   최신 소스를 다시 빌드한 뒤 실행하세요.")
        return None

    compiled_sources = compiled_source_files(elf_path)
    has2_sources = [
        path for path in compiled_sources if os.path.basename(path) == "HAS2_Wifi.cpp"
    ]
    if not has2_sources:
        print("❌ ELF에서 실제 사용된 HAS2_Wifi 소스 경로를 확인할 수 없습니다.")
        print("   디버그 정보가 포함된 전체 빌드를 다시 생성하세요.")
        return None

    dependency_paths, missing_dependency_paths = dependency_files_from_map(map_path)
    if not dependency_paths or missing_dependency_paths:
        print("❌ 이 빌드의 컴파일 의존성 파일이 없거나 불완전합니다.")
        if missing_dependency_paths:
            print(f"   누락된 파일: {missing_dependency_paths[0]}")
        print("   Arduino 빌드 캐시를 지우지 말고 깨끗하게 다시 빌드하세요.")
        return None
    dependency_files = []
    for dependency_path in dependency_paths:
        dependency_files.extend(read_make_dependencies(dependency_path))
    missing_inputs = [path for path in dependency_files if not os.path.isfile(path)]
    if missing_inputs:
        print("❌ 빌드에 사용된 소스 또는 헤더가 삭제되었거나 이동했습니다.")
        print(f"   누락된 입력: {missing_inputs[0]}")
        print("   현재 의존성으로 깨끗하게 다시 빌드하세요.")
        return None
    has2_headers = [
        path for path in dependency_files if os.path.basename(path) == "HAS2_Wifi.h"
    ]
    if not has2_headers or os.path.dirname(has2_headers[0]) != os.path.dirname(has2_sources[0]):
        print("❌ ELF와 컴파일 의존성의 HAS2_Wifi 경로가 일치하지 않습니다.")
        print("   빌드 캐시를 정리하고 동일한 라이브러리로 다시 빌드하세요.")
        return None

    dependency_files.extend(compiled_sources)
    dependency_files.extend(dependency_paths)
    newer_dependencies = [
        path
        for path in dependency_files
        if os.path.getmtime(path) > os.path.getmtime(app_bin)
    ]
    if newer_dependencies:
        newest_dependency = max(newer_dependencies, key=os.path.getmtime)
        print("❌ 빌드 이후 라이브러리 소스가 변경되어 업로드를 중단했습니다.")
        print(f"   변경된 의존성: {newest_dependency}")
        print("   현재 라이브러리로 깨끗하게 다시 빌드하세요.")
        return None

    try:
        with open(map_path, encoding="utf-8", errors="ignore") as map_file:
            has_three_arg_situation = THREE_ARG_SITUATION_SYMBOL in map_file.read()
    except OSError:
        has_three_arg_situation = False
    if not has_three_arg_situation:
        print("❌ 빌드 결과에서 HAS2_Wifi 3인자 Situation 구현을 확인할 수 없습니다.")
        print("   value=술래, key=생존자 규칙을 지원하는 라이브러리로 다시 빌드하세요.")
        return None

    secrets_header = os.path.join(BASE_DIR, "secrets.h")
    deploy_secrets = os.path.join(BASE_DIR, "scripts", "secrets.py")
    firmware_secret = read_hmac_secret(secrets_header)
    signing_secret = read_hmac_secret(deploy_secrets)
    if firmware_secret is None:
        print(f"❌ OTA 비밀키를 읽을 수 없습니다: {secrets_header}")
        return None
    if signing_secret is None:
        print(f"❌ OTA 서명 키를 읽을 수 없습니다: {deploy_secrets}")
        return None
    if PLACEHOLDER_SECRET in (firmware_secret, signing_secret):
        print("❌ OTA HMAC 키가 예제 값인 상태라 업로드를 중단했습니다.")
        print("   실제 장치와 동일한 HMAC 키를 설정한 뒤 깨끗하게 다시 빌드하세요.")
        return None
    if firmware_secret != signing_secret:
        print("❌ secrets.h와 scripts/secrets.py의 HMAC 키가 서로 다릅니다.")
        print("   펌웨어 검증 키와 OTA 서명 키를 동일하게 맞춘 뒤 다시 빌드하세요.")
        return None

    firmware_version = read_firmware_version()
    version_path = os.path.join(BASE_DIR, "version.txt")
    try:
        with open(version_path, encoding="utf-8") as version_file:
            published_version = version_file.read().strip()
    except OSError:
        published_version = ""
    if firmware_version and published_version and firmware_version != published_version:
        print("❌ 펌웨어 버전과 version.txt가 일치하지 않습니다.")
        print(f"   펌웨어: v{firmware_version}, version.txt: v{published_version}")
        return None

    size = os.path.getsize(app_bin)
    modified = time.strftime("%Y-%m-%d %H:%M", time.localtime(os.path.getmtime(app_bin)))
    with open(app_bin, "rb") as binary_file:
        digest = hashlib.sha256(binary_file.read()).hexdigest()
    print(f"📦 펌웨어: v{firmware_version or '?'} / {size:,} bytes / 빌드 {modified}")
    print(f"🔐 SHA-256: {digest}")
    print(f"📚 HAS2_Wifi: {os.path.dirname(has2_sources[0])}")
    return app_bin


def main() -> int:
    args = parse_args()
    if args.wait_timeout < 0:
        print("❌ --wait-timeout은 0 이상이어야 합니다.")
        return 2

    if validate_build() is None:
        return 1

    if args.dry_run and not args.port and not find_ports():
        print("ℹ️  dry-run: 연결된 보드가 없어 빌드 검사만 완료했습니다.")
        return 0

    port = select_port(args.port, 0 if args.dry_run else args.wait_timeout)
    if port is None:
        return 1

    holder = port_holder(port)
    if holder:
        print(f"❌ 포트를 다른 프로그램이 사용 중입니다: {port}")
        print(f"   {holder}")
        print("   Arduino IDE/CoolTerm 시리얼 모니터를 닫고 다시 실행하세요.")
        return 1

    command = [
        "arduino-cli",
        "upload",
        "--fqbn",
        FQBN,
        "--port",
        port,
        "--input-dir",
        BUILD_DIR,
        "--verify",
    ]
    print(f"🔍 포트: {port}")
    if args.dry_run:
        print("✅ dry-run 완료: 실제 플래시는 실행하지 않았습니다.")
        print("   " + " ".join(command))
        return 0

    if not args.yes:
        print("⚠️  이 작업은 선택한 장치의 부트로더와 파티션을 포함해 전체 플래시합니다.")
        try:
            confirmation = input("   대상이 TTGO T1 덕트 보드이면 UPLOAD를 입력하세요: ")
        except (EOFError, KeyboardInterrupt):
            print("\n❌ 사용자 확인을 받지 못해 중단했습니다.")
            return 1
        if confirmation.strip() != "UPLOAD":
            print("❌ 사용자 확인이 일치하지 않아 업로드를 취소했습니다.")
            return 1

    print("⚡ 전체 펌웨어 업로드 시작...")
    result = subprocess.run(command, cwd=BASE_DIR, check=False)
    if result.returncode != 0:
        print("❌ 업로드 실패. BOOT 버튼을 누른 채 다시 시도해 보세요.")
        return result.returncode

    print("✅ 업로드 완료")
    print(f"   시리얼 모니터: arduino-cli monitor -p {port} -c baudrate=115200")
    return 0


if __name__ == "__main__":
    sys.exit(main())
