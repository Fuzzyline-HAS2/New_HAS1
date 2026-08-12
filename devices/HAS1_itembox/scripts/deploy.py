import sys
import os
import glob

sys.stdout.reconfigure(encoding='utf-8')

_base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_home = os.path.expanduser("~")

# SecureOTA 라이브러리(= deploy_core.py 보유)를 탐색한다.
_lib_roots = [
    os.path.join(_home, "Documents", "Arduino", "libraries"),
    os.path.join(_home, "OneDrive", "Documents", "Arduino", "libraries"),
    os.path.join(_home, "OneDrive", "문서", "Arduino", "libraries"),
    os.path.join(_home, "Arduino", "libraries"),
    os.path.join(_base_dir, ".."),
    os.path.join(_base_dir, "..", "CODE"),
    os.path.join(_base_dir, "..", ".."),
    os.path.join(_base_dir, "..", "..", "CODE"),
]


def _find_scripts_dir():
    for _root in _lib_roots:
        if not os.path.isdir(_root):
            continue
        for _core in glob.glob(os.path.join(_root, "SecureOTA*", "scripts", "deploy_core.py")):
            return os.path.dirname(_core)
    return None


_scripts_dir = _find_scripts_dir()
if _scripts_dir is None:
    print("❌ SecureOTA 라이브러리(deploy_core.py)를 찾을 수 없습니다.")
    print("   최신 SecureOTA 를 Arduino libraries 에 설치/갱신하세요:")
    print("   https://github.com/Fuzzyline-HAS2/SecureOTA.git")
    sys.exit(1)

sys.path.insert(0, _scripts_dir)
from deploy_core import run_deploy
run_deploy(_base_dir)
