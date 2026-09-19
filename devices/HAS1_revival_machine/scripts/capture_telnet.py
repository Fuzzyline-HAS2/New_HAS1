#!/usr/bin/env python3
"""HAS1_revival_machine 텔넷 콘솔을 호스트 시각과 함께 파일로 캡처한다.

기기는 로그에 타임스탬프를 찍지 않으므로(telnet.ino는 Serial을 그대로 미러링할 뿐이다),
지연을 측정하려면 받는 쪽에서 줄마다 시각을 붙여야 한다. 이 스크립트가 그 역할을 한다.

기기의 텔넷 서버는 동시 접속 1개만 허용한다(telnet.ino TelnetRun). PuTTY 등 다른 텔넷
창이 열려 있으면 "Telnet already connected."만 받고 끊기므로 먼저 닫아야 한다.

사용법:
    python3 capture_telnet.py --out run_A_held.log

    실행 중 콘솔에 문구를 입력하고 Enter를 치면 그 시점에 마커 줄이 삽입된다.
    시행 구분용으로 쓴다(예: "A1 hold start", "A1 hold end").
    q + Enter 로 종료.

출력 형식 (analyze_capture.py가 읽는 형식):
    2026-09-19T06:12:31.482123  [RFID] Tag detected - sending situation to server: G1P1
    ISO 타임스탬프 + 공백 2칸 + 기기가 출력한 원본 줄.
"""

import argparse
import errno
import socket
import sys
import threading
from datetime import datetime

DEFAULT_HOST = "172.30.1.9"
DEFAULT_PORT = 23

_stop = threading.Event()
_lock = threading.Lock()


def stamp() -> str:
    return datetime.now().isoformat(timespec="microseconds")


def emit(sink, text: str, echo: bool = True) -> None:
    line = f"{stamp()}  {text}"
    with _lock:
        sink.write(line + "\n")
        sink.flush()
        if echo:
            print(line, flush=True)


def read_markers(sink) -> None:
    """표준입력으로 시행 구분 마커를 넣는다. q 입력 시 종료."""
    for raw in sys.stdin:
        text = raw.strip()
        if text.lower() in ("q", "quit", "exit"):
            emit(sink, "### MARK stop requested")
            _stop.set()
            return
        emit(sink, f"### MARK {text}" if text else "### MARK")


def connect(host: str, port: int, timeout: float) -> socket.socket:
    sock = socket.create_connection((host, port), timeout=timeout)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(1.0)
    return sock


def pump(sock: socket.socket, sink) -> str:
    """연결이 끊길 때까지 줄 단위로 받아 기록한다. 종료 사유를 반환한다."""
    buffer = b""
    while not _stop.is_set():
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        except OSError as exc:
            return f"socket error: {exc}"
        if not chunk:
            return "closed by device"
        buffer += chunk
        while b"\n" in buffer:
            raw, buffer = buffer.split(b"\n", 1)
            text = raw.replace(b"\r", b"").decode("utf-8", "replace")
            if "Telnet already connected" in text:
                emit(sink, "### MARK 기기가 접속을 거부했다 - 다른 텔넷 창을 닫고 다시 실행하라")
                return "already connected"
            emit(sink, text)
    # 개행 없이 남은 꼬리도 버리지 않는다.
    if buffer:
        emit(sink, buffer.replace(b"\r", b"").decode("utf-8", "replace"))
    return "stopped"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--out", required=True, help="캡처 파일 경로")
    parser.add_argument("--connect-timeout", type=float, default=5.0)
    parser.add_argument("--no-reconnect", action="store_true", help="끊기면 재접속하지 않고 종료")
    args = parser.parse_args()

    try:
        sink = open(args.out, "a", encoding="utf-8")
    except OSError as exc:
        print(f"출력 파일을 열 수 없다: {exc}", file=sys.stderr)
        return 2

    with sink:
        emit(sink, f"### MARK capture start host={args.host}:{args.port}")
        print("마커를 넣으려면 문구 입력 후 Enter, 종료는 q + Enter", flush=True)

        reader = threading.Thread(target=read_markers, args=(sink,), daemon=True)
        reader.start()

        backoff = 1.0
        connected_once = False
        while not _stop.is_set():
            try:
                sock = connect(args.host, args.port, args.connect_timeout)
            except OSError as exc:
                reason = exc.strerror or str(exc)
                if exc.errno == errno.ECONNREFUSED:
                    reason = "연결 거부 - 기기가 부팅 중이거나 Wi-Fi가 끊겼을 수 있다"
                elif isinstance(exc, socket.timeout):
                    reason = "연결 시간초과 - 같은 네트워크에 있는지, IP가 맞는지 확인하라"
                emit(sink, f"### MARK connect failed: {reason}")
                if args.no_reconnect or not connected_once:
                    if not connected_once:
                        print(
                            "\n첫 접속에 실패했다. 이 호스트가 기기와 같은 Wi-Fi(172.30.1.0/24)에 있어야 한다.",
                            file=sys.stderr,
                        )
                        return 1
                    return 1
                _stop.wait(backoff)
                backoff = min(backoff * 2, 16.0)
                continue

            connected_once = True
            backoff = 1.0
            emit(sink, "### MARK connected")
            reason = pump(sock, sink)
            try:
                sock.close()
            except OSError:
                pass
            emit(sink, f"### MARK disconnected ({reason})")

            if reason == "already connected":
                return 1
            if args.no_reconnect or _stop.is_set():
                break
            _stop.wait(1.0)

        emit(sink, "### MARK capture end")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n중단됨", file=sys.stderr)
        sys.exit(130)
