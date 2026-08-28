"""
HAS2_Wifi Mock Server - has2.php 동작을 흉내냅니다.
하드웨어(ESP32) 연동 통합 테스트 시 사용합니다.

사용법:
    python mock_server.py

ESP32에서 서버 주소를 PC IP로 변경:
    HAS1_escape_main.h: HAS2_Wifi has2wifi("http://<PC_IP>:5000");
"""

from flask import Flask, request, jsonify
import json

app = Flask(__name__)

# ── 인메모리 상태 DB ──────────────────────────────────────────────────────────

_devices: dict = {
    "HAS1_escape_main": {
        "device_name": "HAS1_escape_main",
        "game_state": "setting",
        "device_state": "setting",
        "manage_state": "mu",
        "device_type": "HAS1_escape_main",
        "watchdog": 0,
    }
}
_tags: dict = {}          # key: device_name → tag JSON
_sent_log: list = []      # ESP32가 Send한 명령 이력
_shift: dict = {"shift_machine": 0, "watchdog": 0}

# ── has2.php 엔드포인트 ───────────────────────────────────────────────────────

@app.route("/has2.php")
def has2_php():
    req = request.args.get("request", "")

    if req == "ReceiveMine":
        # MAC 무관하게 첫 번째 디바이스 반환 (테스트 단순화)
        return json.dumps(list(_devices.values())[0])

    elif req == "Receive":
        key = request.args.get("key", "")
        return json.dumps(_tags.get(key, {"role": "tagger"}))

    elif req == "Send":
        key = request.args.get("key", "")
        column = request.args.get("column", "")
        value = request.args.get("value", "")
        if key in _devices:
            _devices[key][column] = value
        _sent_log.append({"key": key, "column": column, "value": value})
        return "1"

    elif req == "Loop":
        return json.dumps(_shift)

    return json.dumps({})

# ── 테스트 제어 API ───────────────────────────────────────────────────────────

@app.route("/test/set_state", methods=["POST"])
def set_state():
    """테스트에서 디바이스 상태를 직접 설정."""
    data = request.json
    device = data.get("device", "HAS1_escape_main")
    if device in _devices:
        _devices[device].update(data.get("state", {}))
    return jsonify({"ok": True})

@app.route("/test/get_device")
def get_device():
    """현재 디바이스 상태 조회."""
    device = request.args.get("device", "HAS1_escape_main")
    return jsonify(_devices.get(device, {}))

@app.route("/test/set_tag", methods=["POST"])
def set_tag():
    """Receive 요청 시 반환할 태그 데이터 설정."""
    data = request.json
    _tags[data["key"]] = data["tag"]
    return jsonify({"ok": True})

@app.route("/test/get_sent")
def get_sent():
    """ESP32가 Send한 명령 이력 반환."""
    return jsonify(_sent_log)

@app.route("/test/clear", methods=["POST"])
def clear():
    """Send 이력 초기화."""
    _sent_log.clear()
    return jsonify({"ok": True})

@app.route("/test/trigger_shift", methods=["POST"])
def trigger_shift():
    """shift_machine=1 → ESP32가 ReceiveMine 재호출하여 상태 갱신."""
    _shift["shift_machine"] = 1
    return jsonify({"ok": True})

@app.route("/test/reset_shift", methods=["POST"])
def reset_shift():
    _shift["shift_machine"] = 0
    return jsonify({"ok": True})

# ── 진입점 ────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    print("Mock HAS2 Server running at http://0.0.0.0:5000/has2.php")
    print("ESP32 서버 주소를 http://<THIS_PC_IP>:5000 으로 변경 후 플래싱하세요.")
    app.run(host="0.0.0.0", port=5000, debug=False)
