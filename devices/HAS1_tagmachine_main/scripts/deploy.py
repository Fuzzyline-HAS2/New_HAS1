import os
import re
import sys
import shutil
import subprocess
import glob

# Windows CMD 이모지 출력을 위한 설정
sys.stdout.reconfigure(encoding='utf-8')

SCRIPT_DIR    = os.path.dirname(os.path.abspath(__file__))
BASE_DIR      = os.path.dirname(SCRIPT_DIR)

# ── 기기 저장소에 맞게 변경 ─────────────────────────────────
SKETCH_FILE   = os.path.join(BASE_DIR, "HAS1_tagmachine_main.ino")  # ← 변경 필요
VERSION_MACRO = "FIRMWARE_VER"
# ────────────────────────────────────────────────────────────

OUTPUT_BIN    = os.path.join(BASE_DIR, "update.bin")
OUTPUT_SIG    = os.path.join(BASE_DIR, "update.sig")
VERSION_TXT   = os.path.join(BASE_DIR, "version.txt")

try:
    sys.path.insert(0, SCRIPT_DIR)
    from secrets import HMAC_SECRET
except ImportError:
    print("❌ 오류: scripts/secrets.py 파일이 없습니다.")
    print("   secrets.py.example 을 secrets.py 로 복사한 뒤 비밀키를 설정하세요.")
    sys.exit(1)

def get_current_version():
    with open(SKETCH_FILE, "r", encoding="utf-8") as f:
        content = f.read()
    pattern = rf'#define\s+{VERSION_MACRO}\s+(\d+)'
    match = re.search(pattern, content)
    if match:
        return int(match.group(1))
    return None

def increment_version(current_ver):
    new_ver = current_ver + 1
    with open(SKETCH_FILE, "r", encoding="utf-8") as f:
        content = f.read()
    new_content = re.sub(
        rf'#define\s+{VERSION_MACRO}\s+\d+',
        f'#define {VERSION_MACRO} {new_ver}',
        content
    )
    with open(SKETCH_FILE, "w", encoding="utf-8") as f:
        f.write(new_content)
    return new_ver

def find_newest_bin():
    sketch_dir = os.path.dirname(SKETCH_FILE)
    sketch_bin_name = os.path.basename(SKETCH_FILE) + ".bin"
    search_patterns = [
        os.path.join(sketch_dir, "build", "**", sketch_bin_name),
        os.path.join(sketch_dir, "**", sketch_bin_name),
        os.path.join(BASE_DIR, "build", "**", sketch_bin_name),
    ]
    candidates = []
    for pattern in search_patterns:
        candidates.extend(glob.glob(pattern, recursive=True))
    candidates = list(dict.fromkeys(candidates))
    sketch_mtime = os.path.getmtime(SKETCH_FILE)
    candidates = [f for f in candidates if os.path.getmtime(f) >= sketch_mtime]
    if not candidates:
        return None
    return max(candidates, key=os.path.getmtime)

def git_push(version):
    print("\n☁️ GitHub 에 업로드 중...")
    try:
        with open(VERSION_TXT, "w", encoding="utf-8") as f:
            f.write(str(version))
        print(f"📝 version.txt → v{version}")
        files_to_add = [
            "update.bin",
            "update.sig",
            "version.txt",
            os.path.relpath(SKETCH_FILE, BASE_DIR).replace("\\", "/"),
        ]
        subprocess.run(["git", "-C", BASE_DIR, "add"] + files_to_add, check=True)
        subprocess.run(
            ["git", "-C", BASE_DIR, "commit", "-m", f"Firmware Update v{version}"],
            check=True
        )
        subprocess.run(["git", "-C", BASE_DIR, "push"], check=True)
        print("✅ GitHub 업로드 완료!")
    except subprocess.CalledProcessError as e:
        print(f"❌ Git 오류: {e}")

def main():
    print("🚀 SecureOTA 배포 자동화 시작...")
    if HMAC_SECRET == "CHANGE_THIS_TO_YOUR_SECRET":
        print("❌ 오류: scripts/secrets.py 의 HMAC_SECRET 을 설정하세요.")
        return
    cur_ver = get_current_version()
    if cur_ver is None:
        print(f"❌ 오류: {SKETCH_FILE} 에서 '#define {VERSION_MACRO}' 를 찾을 수 없습니다.")
        return
    print(f"\n현재 버전: v{cur_ver}")
    new_ver = increment_version(cur_ver)
    print(f"🔼 버전 변경: v{cur_ver} → v{new_ver}")
    print("\n⏳ [행동 필요] 아두이노 IDE 에서 Ctrl+Alt+S 를 실행하세요.")
    print("   완료되면 Enter 를 누르세요...")
    input()
    print("🔎 빌드 파일 탐색 중...")
    bin_file = find_newest_bin()
    if not bin_file:
        print("❌ .bin 파일을 찾을 수 없습니다.")
        return
    print(f"   발견: {os.path.relpath(bin_file, BASE_DIR)}")
    try:
        shutil.copy2(bin_file, OUTPUT_BIN)
        print(f"📦 → update.bin 복사 완료")
    except Exception as e:
        print(f"❌ 파일 복사 실패: {e}")
        return
    sign_script = os.path.join(SCRIPT_DIR, "sign_firmware.py")
    result = subprocess.run(
        [sys.executable, sign_script, OUTPUT_BIN, HMAC_SECRET, OUTPUT_SIG],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        print(f"❌ 서명 실패:\n{result.stderr}")
        return
    print("🔏 서명 완료 → update.sig")
    git_push(new_ver)
    print(f"\n🎉 배포 완료! v{new_ver} 이(가) GitHub 에 업로드되었습니다.")

if __name__ == "__main__":
    main()
