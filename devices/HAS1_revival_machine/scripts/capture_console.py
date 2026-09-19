#!/usr/bin/env python3
"""HAS1_revival_machine 콘솔을 호스트 시각과 함께 파일로 캡처한다. USB 시리얼 또는 텔넷.

기기는 로그에 타임스탬프를 찍지 않으므로(telnet.ino는 Serial을 그대로 미러링할 뿐이다),
지연을 측정하려면 받는 쪽에서 줄마다 시각을 붙여야 한다. 이 스크립트가 그 역할을 한다.

USB 시리얼을 권장한다:
  - 기기의 텔넷 슬롯(동시 1개)을 쓰지 않는다. 텔넷 창을 따로 띄워둘 수 있다.
  - 부팅부터 잡힌다. 텔넷은 TelnetInit()이 Wi-Fi 연결 후에 시작되므로 그 전 로그를 놓친다.
  - Wi-Fi가 끊기거나 ESP가 재시작해도 끊기지 않는다. 혼잡이 용의선상에 있는 이번 측정에서는
    바로 그 순간이 가장 중요한데, 텔넷 캡처는 그때 눈이 먼다.
  - TelnetDebugConsole::write는 항상 HardwareDebugSerial에 먼저 쓰므로(telnet.ino) USB 쪽이
    텔넷보다 적게 나오는 일은 없다.

사용법:
    python3 capture_console.py --serial /dev/ttyUSB0 --out run.log     # Linux
    python3 capture_console.py --serial COM3 --out run.log             # Windows
    python3 capture_console.py --telnet 172.30.1.9 --out run.log       # 텔넷

    실행 중 콘솔에 문구를 입력하고 Enter를 치면 그 시점에 마커 줄이 삽입된다.
    시행 구분용으로 쓴다(예: "A1 hold", "B1 release"). q + Enter 로 종료.

시리얼 주의:
    포트를 여는 순간 DTR/RTS가 걸리면 ESP32가 리셋된다. 기본으로 둘 다 눌러 막지만,
    보드/드라이버에 따라 그래도 한 번 리셋될 수 있다. 리셋은 로그에 부팅 줄로 남고
    분석기가 경고하므로, 시행을 시작하기 전에 부팅이 끝난 것을 확인하고 진행하면 된다.
    포트 이름은 Linux `ls /dev/ttyUSB* /dev/ttyACM*`, macOS `ls /dev/cu.*`,
    Windows 장치 관리자에서 확인한다.

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

DEFAULT_TELNET_HOST = "172.30.1.9"
TELNET_PORT = 23
# HAS1_revival_machine.ino:50 Serial.begin(115200)
DEFAULT_BAUD = 115200

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


class TelnetSource:
    """기기의 텔넷 서버(포트 23). 동시 접속 1개만 허용한다(telnet.ino TelnetRun)."""

    def __init__(self, host, port, connect_timeout):
        self.host = host
        self.port = port
        self.connect_timeout = connect_timeout
        self.sock = None

    def describe(self):
        return f"telnet {self.host}:{self.port}"

    def open(self):
        self.sock = socket.create_connection((self.host, self.port), timeout=self.connect_timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.settimeout(1.0)

    def read(self):
        """바이트를 반환한다. 읽을 게 없으면 b"" (타임아웃), 끊기면 None."""
        try:
            chunk = self.sock.recv(4096)
        except socket.timeout:
            return b""
        return chunk if chunk else None

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def explain(self, exc):
        if isinstance(exc, socket.timeout):
            return "연결 시간초과 - 같은 네트워크에 있는지, IP가 맞는지 확인하라"
        if getattr(exc, "errno", None) == errno.ECONNREFUSED:
            return "연결 거부 - 기기가 부팅 중이거나 Wi-Fi가 끊겼을 수 있다"
        return exc.strerror or str(exc)


class SerialSource:
    """USB 시리얼(UART0). 텔넷 슬롯을 쓰지 않고 부팅부터 잡힌다."""

    def __init__(self, port, baud, allow_reset):
        self.port = port
        self.baud = baud
        self.allow_reset = allow_reset
        self.ser = None

    def describe(self):
        return f"serial {self.port}@{self.baud}"

    def open(self):
        try:
            import serial  # pyserial
        except ImportError as exc:
            raise SystemExit(
                "pyserial이 필요하다:  python3 -m pip install pyserial\n"
                f"(원인: {exc})") from exc

        def build(guard_reset):
            handle = serial.Serial()
            if guard_reset:
                # 포트를 여는 순간 DTR/RTS가 걸리면 ESP32가 리셋된다. pyserial은 port 없이
                # 만든 객체의 dtr/rts 상태를 기억해 open() 시점에 적용하므로, 열기 전에 내려둔다.
                handle.dtr = False
                handle.rts = False
            handle.dsrdtr = False
            handle.rtscts = False
            handle.xonxoff = False
            handle.baudrate = self.baud
            handle.timeout = 1.0
            handle.port = self.port
            handle.open()
            return handle

        guard = not self.allow_reset
        try:
            self.ser = build(guard)
        except OSError as exc:
            # 일부 드라이버와 가상 포트는 모뎀 제어선 ioctl을 거부한다. 리셋 방지보다
            # 캡처가 되는 쪽이 중요하므로 한 번은 그대로 열어본다.
            if not guard:
                raise
            print(f"[warn] DTR/RTS 억제 실패({exc}) - 억제 없이 다시 연다. "
                  "보드가 한 번 리셋될 수 있다.", file=sys.stderr)
            self.ser = build(False)

    def read(self):
        try:
            waiting = self.ser.in_waiting
        except OSError:
            return None
        try:
            # 1바이트를 timeout까지 기다렸다가, 이미 쌓인 나머지를 한 번에 가져온다.
            chunk = self.ser.read(waiting or 1)
        except OSError:
            return None
        return chunk

    def close(self):
        if self.ser is not None:
            try:
                self.ser.close()
            except OSError:
                pass
            self.ser = None

    def explain(self, exc):
        message = getattr(exc, "strerror", None) or str(exc)
        if "could not open port" in str(exc).lower() or getattr(exc, "errno", None) == errno.ENOENT:
            return (f"포트를 열 수 없다 ({self.port}). 이름이 맞는지, 다른 프로그램"
                    "(아두이노 시리얼 모니터 등)이 잡고 있지 않은지 확인하라")
        if getattr(exc, "errno", None) == errno.EACCES:
            return (f"권한이 없다 ({self.port}). Linux라면 dialout 그룹에 넣거나 "
                    "sudo로 실행하라")
        return message


def pump(source, sink) -> str:
    """연결이 끊길 때까지 줄 단위로 받아 기록한다. 종료 사유를 반환한다."""
    buffer = b""
    while not _stop.is_set():
        try:
            chunk = source.read()
        except OSError as exc:
            return f"io error: {exc}"
        if chunk is None:
            return "closed by device"
        if not chunk:
            continue
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
    transport = parser.add_mutually_exclusive_group()
    transport.add_argument("--serial", metavar="PORT",
                           help="USB 시리얼 포트 (예: /dev/ttyUSB0, /dev/cu.usbserial-0001, COM3)")
    transport.add_argument("--telnet", nargs="?", const=DEFAULT_TELNET_HOST, metavar="HOST",
                           help=f"텔넷으로 붙는다 (기본 호스트 {DEFAULT_TELNET_HOST})")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"시리얼 속도 (기본 {DEFAULT_BAUD})")
    parser.add_argument("--telnet-port", type=int, default=TELNET_PORT,
                        help=f"텔넷 포트 (기본 {TELNET_PORT})")
    parser.add_argument("--out", required=True, help="캡처 파일 경로")
    parser.add_argument("--connect-timeout", type=float, default=5.0)
    parser.add_argument("--no-reconnect", action="store_true", help="끊기면 재접속하지 않고 종료")
    parser.add_argument("--allow-serial-reset", action="store_true",
                        help="시리얼 열 때 DTR/RTS를 그대로 둔다 (보드가 리셋될 수 있다)")
    args = parser.parse_args()

    if args.serial:
        source = SerialSource(args.serial, args.baud, args.allow_serial_reset)
    else:
        source = TelnetSource(args.telnet or DEFAULT_TELNET_HOST, args.telnet_port, args.connect_timeout)

    try:
        sink = open(args.out, "a", encoding="utf-8")
    except OSError as exc:
        print(f"출력 파일을 열 수 없다: {exc}", file=sys.stderr)
        return 2

    with sink:
        emit(sink, f"### MARK capture start {source.describe()}")
        print("마커를 넣으려면 문구 입력 후 Enter, 종료는 q + Enter", flush=True)

        reader = threading.Thread(target=read_markers, args=(sink,), daemon=True)
        reader.start()

        backoff = 1.0
        connected_once = False
        while not _stop.is_set():
            try:
                source.open()
            except OSError as exc:
                emit(sink, f"### MARK connect failed: {source.explain(exc)}")
                if not connected_once:
                    if isinstance(source, TelnetSource):
                        print("\n첫 접속에 실패했다. 이 호스트가 기기와 같은 Wi-Fi"
                              "(172.30.1.0/24)에 있어야 한다.", file=sys.stderr)
                    else:
                        print("\n포트를 열지 못했다. 사용 가능한 포트를 확인하라:\n"
                              "  Linux  : ls /dev/ttyUSB* /dev/ttyACM*\n"
                              "  macOS  : ls /dev/cu.*\n"
                              "  Windows: 장치 관리자 > 포트(COM & LPT)", file=sys.stderr)
                    return 1
                if args.no_reconnect:
                    return 1
                _stop.wait(backoff)
                backoff = min(backoff * 2, 16.0)
                continue

            connected_once = True
            backoff = 1.0
            emit(sink, "### MARK connected")
            reason = pump(source, sink)
            source.close()
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
