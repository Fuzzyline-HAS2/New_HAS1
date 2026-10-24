"""Windows-service helpers, also used by console mode to prevent duplicate writers.

No service-management privileges or third-party Windows modules are needed here.
"""
from contextlib import AbstractContextManager
import ctypes
import hashlib
import json
from ctypes import wintypes
import os
from pathlib import Path


class InstanceLock(AbstractContextManager):
    """OS-released lock; a stale lock file after a crash is harmless."""
    def __init__(self, database):
        self.path = Path(str(Path(database).resolve()) + ".lock")
        self.handle = None

    def __enter__(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        handle = self.path.open("a+b")
        try:
            if os.name == "nt":
                import msvcrt
                handle.seek(0, 2)
                if handle.tell() == 0:
                    handle.write(b"\0")
                    handle.flush()
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            handle.close()
            raise RuntimeError("Another collector already owns this database") from exc
        self.handle = handle
        return self

    def __exit__(self, *args):
        if self.handle is not None:
            self.handle.close()  # The OS releases its lock, including on abnormal exit.
            self.handle = None


class BrokerInstanceLock(AbstractContextManager):
    """Cross-session Windows guard, including consoles using a different DB.

    CreateMutex existence (or access denied on an existing user's object) means
    another collector is active. The kernel removes it after the last handle
    closes, including crashes. No globally writable credential/control channel.
    """
    def __init__(self, config):
        identity = json.dumps([config["host"].lower().rstrip("."), config["port"],
                               config["client_id"]], separators=(",", ":"))
        self.name = "Global\\HAS1BlackBox_" + hashlib.sha256(identity.encode()).hexdigest()
        self.handle = None
        self.kernel32 = None

    def __enter__(self):
        if os.name != "nt":
            return self
        self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel32.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]
        self.kernel32.CreateMutexW.restype = wintypes.HANDLE
        self.kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel32.CloseHandle.restype = wintypes.BOOL
        ctypes.set_last_error(0)
        handle = self.kernel32.CreateMutexW(None, False, self.name)
        error = ctypes.get_last_error()
        if handle and error == 183:  # ERROR_ALREADY_EXISTS
            self.kernel32.CloseHandle(handle)
            raise RuntimeError("Another collector already uses this MQTT broker/client_id")
        if not handle:
            if error == 5:  # Existing mutex owned by another account: fail closed.
                raise RuntimeError("MQTT collector instance lock is owned by another account")
            raise OSError(error, "Cannot create global MQTT collector instance lock")
        self.handle = handle
        return self

    def __exit__(self, *args):
        if self.handle:
            self.kernel32.CloseHandle(self.handle)
            self.handle = None


def read_machine_password(path):
    """Read a .NET ProtectedData/LocalMachine UTF-8 blob (ACL protection required)."""
    if os.name != "nt":
        raise RuntimeError("Windows DPAPI credentials require Windows")
    raw = Path(path).read_bytes()
    if not 1 <= len(raw) <= 16384:
        raise ValueError("Invalid credential file size")

    class Blob(ctypes.Structure):
        _fields_ = [("length", wintypes.DWORD), ("data", ctypes.POINTER(ctypes.c_ubyte))]

    crypt32 = ctypes.WinDLL("crypt32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    crypt32.CryptUnprotectData.argtypes = [ctypes.POINTER(Blob), ctypes.c_void_p,
                                          ctypes.c_void_p, ctypes.c_void_p,
                                          ctypes.c_void_p, wintypes.DWORD,
                                          ctypes.POINTER(Blob)]
    crypt32.CryptUnprotectData.restype = wintypes.BOOL
    kernel32.LocalFree.argtypes = [ctypes.c_void_p]
    kernel32.LocalFree.restype = ctypes.c_void_p
    storage = (ctypes.c_ubyte * len(raw)).from_buffer_copy(raw)
    source = Blob(len(raw), storage)
    output = Blob()
    if not crypt32.CryptUnprotectData(ctypes.byref(source), None, None, None, None,
                                     1, ctypes.byref(output)):  # UI_FORBIDDEN
        raise OSError("Cannot decrypt service credential on this Windows machine")
    try:
        password = ctypes.string_at(output.data, output.length).decode("utf-8")
        if not password or "\0" in password:
            raise ValueError("Invalid service credential")
        return password
    finally:
        ctypes.memset(output.data, 0, output.length)
        kernel32.LocalFree(output.data)
