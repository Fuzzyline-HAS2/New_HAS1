import sys
import os

sys.stdout.reconfigure(encoding='utf-8')

_base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# SecureOTA 라이브러리에서 deploy_core 탐색
# (Arduino 표준 설치 경로 -> 형제 폴더 순으로 시도)
_search_paths = [
    os.path.join(os.path.expanduser("~"), "Documents", "Arduino", "libraries", "SecureOTA", "scripts"),
    os.path.join(os.path.expanduser("~"), "Arduino", "libraries", "SecureOTA", "scripts"),
    os.path.join(_base_dir, "..", "SecureOTA", "scripts"),  # 형제 폴더 (개발 환경)
]

for _path in _search_paths:
    if os.path.exists(os.path.join(_path, "deploy_core.py")):
        sys.path.insert(0, _path)
        break
else:
    print("SecureOTA 라이브러리를 찾을 수 없습니다.")
    print("Arduino IDE 에서 SecureOTA 라이브러리를 설치하세요.")
    sys.exit(1)

from deploy_core import run_deploy
run_deploy(_base_dir)
