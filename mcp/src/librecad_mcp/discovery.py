# SPDX-License-Identifier: GPL-2.0-or-later
"""The Remote API discovery file LibreCAD writes while its API is listening.

LibreCAD (the LibreCAD-mcp build) writes ``remote-api.json`` into its per-user application data
folder when Preferences > Remote API (MCP) is enabled, and removes it when the API stops. It holds
``{url, port, token, pid, app, app_version, mcp_build, api_version, started_at}``. Reading it lets
this server connect with zero configuration on the same machine (DESIGN.md section 1.4).
"""
from __future__ import annotations

import json
import os
import sys
from collections.abc import Mapping
from pathlib import Path

DISCOVERY_FILE_NAME = "remote-api.json"
# QStandardPaths::AppDataLocation is <data dir>/<organization>/<application>.
_ORGANIZATION = "LibreCAD"
_APPLICATION = "LibreCAD-2"


def default_paths(platform: str | None = None, env: Mapping[str, str] | None = None,
                  home: Path | None = None) -> list[Path]:
    """Where LibreCAD writes the discovery file on this OS, most likely first.

    Windows: %APPDATA%/LibreCAD/LibreCAD-2; Linux and other Unix: $XDG_DATA_HOME (default
    ~/.local/share)/LibreCAD/LibreCAD-2; macOS: ~/Library/Application Support/LibreCAD/LibreCAD-2.
    """
    platform = platform or sys.platform
    env = os.environ if env is None else env
    home = home if home is not None else Path(os.path.expanduser("~"))
    if platform.startswith("win"):
        appdata = (env.get("APPDATA") or "").strip()
        base = Path(appdata) if appdata else home / "AppData" / "Roaming"
    elif platform == "darwin":
        base = home / "Library" / "Application Support"
    else:
        xdg = (env.get("XDG_DATA_HOME") or "").strip()
        # The XDG spec ignores a relative XDG_DATA_HOME, and so does Qt.
        base = Path(xdg) if xdg.startswith("/") else home / ".local" / "share"
        paths = [base / _ORGANIZATION / _APPLICATION / DISCOVERY_FILE_NAME]
        # A snap's HOME is ~/snap/<name>/<revision>: its file lands there (newest first).
        snaps = sorted((home / "snap").glob(f"librecad*/current/.local/share/{_ORGANIZATION}/{_APPLICATION}/"
                                            f"{DISCOVERY_FILE_NAME}"),
                       key=_mtime, reverse=True)
        return paths + [p for p in snaps if p not in paths]
    return [base / _ORGANIZATION / _APPLICATION / DISCOVERY_FILE_NAME]


def _mtime(path: Path) -> float:
    try:
        return path.stat().st_mtime
    except OSError:
        return 0.0


# How much later than the file's started_at the process may seem to have started (clock
# rounding: started_at has whole seconds) before it counts as another process with a reused pid.
_START_SLACK_S = 5.0
_STILL_ACTIVE = 259
_ERROR_INVALID_PARAMETER = 87


class _Process:
    """What this machine says about a process id: alive (None: cannot be told), when it started
    (seconds since the epoch) and the file name of its program, each None when unknown."""

    def __init__(self, alive: bool | None, started: float | None = None, image: str | None = None):
        self.alive, self.started, self.image = alive, started, image


def _filetime_to_epoch(value: int) -> float:
    return (value - 116444736000000000) / 1e7


def _windows_process(pid: int) -> _Process:
    import ctypes
    from ctypes import wintypes
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel32.GetExitCodeProcess.restype = wintypes.BOOL
    kernel32.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    kernel32.GetProcessTimes.restype = wintypes.BOOL
    kernel32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                    ctypes.POINTER(wintypes.DWORD)]
    kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        if ctypes.get_last_error() == _ERROR_INVALID_PARAMETER:
            return _Process(False)  # no such process
        # it exists, but this user may not open it (another user's, or an elevated one): the
        # system's process list still tells when it started and what it runs
        return _windows_process_from_list(pid) or _Process(None)
    try:
        code = wintypes.DWORD()
        if kernel32.GetExitCodeProcess(handle, ctypes.byref(code)) and code.value != _STILL_ACTIVE:
            return _Process(False)
        started = None
        times = [wintypes.FILETIME() for _ in range(4)]
        if kernel32.GetProcessTimes(handle, *[ctypes.byref(t) for t in times]):
            started = _filetime_to_epoch((times[0].dwHighDateTime << 32) | times[0].dwLowDateTime)
        image = None
        size = wintypes.DWORD(32768)
        buffer = ctypes.create_unicode_buffer(size.value)
        if kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
            image = buffer.value
        return _Process(True, started, image)
    finally:
        kernel32.CloseHandle(handle)


def _windows_process_from_list(pid: int) -> _Process | None:
    """The process from NtQuerySystemInformation's process list, which every user may read."""
    import ctypes
    from ctypes import wintypes

    class UnicodeString(ctypes.Structure):
        _fields_ = [("Length", wintypes.USHORT), ("MaximumLength", wintypes.USHORT), ("Buffer", ctypes.c_void_p)]

    class ProcessInformation(ctypes.Structure):  # the documented head of SYSTEM_PROCESS_INFORMATION
        _fields_ = [("NextEntryOffset", wintypes.ULONG), ("NumberOfThreads", wintypes.ULONG),
                    ("WorkingSetPrivateSize", ctypes.c_longlong), ("HardFaultCount", wintypes.ULONG),
                    ("NumberOfThreadsHighWatermark", wintypes.ULONG), ("CycleTime", ctypes.c_ulonglong),
                    ("CreateTime", ctypes.c_longlong), ("UserTime", ctypes.c_longlong),
                    ("KernelTime", ctypes.c_longlong), ("ImageName", UnicodeString),
                    ("BasePriority", ctypes.c_long), ("UniqueProcessId", ctypes.c_void_p)]

    try:
        ntdll = ctypes.WinDLL("ntdll")
        query = ntdll.NtQuerySystemInformation
        query.argtypes = [wintypes.ULONG, ctypes.c_void_p, wintypes.ULONG, ctypes.POINTER(wintypes.ULONG)]
        query.restype = ctypes.c_long
        size = 1 << 20
        for _ in range(6):
            buffer = ctypes.create_string_buffer(size)
            needed = wintypes.ULONG(0)
            status = query(5, buffer, size, ctypes.byref(needed))  # SystemProcessInformation
            if status == 0:
                break
            if status & 0xFFFFFFFF != 0xC0000004:  # STATUS_INFO_LENGTH_MISMATCH
                return None
            size = max(size * 2, needed.value + (1 << 16))
        else:
            return None
        offset = 0
        while offset + ctypes.sizeof(ProcessInformation) <= size:
            info = ProcessInformation.from_buffer(buffer, offset)
            if (info.UniqueProcessId or 0) == pid:
                image = (ctypes.wstring_at(info.ImageName.Buffer, info.ImageName.Length // 2)
                         if info.ImageName.Buffer and info.ImageName.Length else None)
                return _Process(True, _filetime_to_epoch(info.CreateTime), image)
            if not info.NextEntryOffset:
                return _Process(False)  # not in the list: not running
            offset += info.NextEntryOffset
    except (OSError, AttributeError, ValueError):
        return None
    return None


def _linux_process(pid: int) -> _Process:
    try:
        with open(f"/proc/{pid}/stat", encoding="ascii", errors="replace") as f:
            stat = f.read()
    except FileNotFoundError:
        return _Process(False)
    except OSError:
        return _Process(None)
    started = None
    try:
        # the fields after the command name, which may hold spaces and parentheses
        fields = stat[stat.rindex(")") + 2:].split()
        if fields and fields[0] in ("Z", "X"):
            return _Process(False)  # a zombie: it has ended
        ticks = int(fields[19])  # field 22, starttime
        with open("/proc/stat", encoding="ascii") as f:
            boot = next(int(line.split()[1]) for line in f if line.startswith("btime "))
        started = boot + ticks / os.sysconf("SC_CLK_TCK")
    except (ValueError, IndexError, StopIteration, OSError):
        started = None
    image = None
    try:
        image = os.path.basename(os.readlink(f"/proc/{pid}/exe"))
    except OSError:
        try:
            with open(f"/proc/{pid}/comm", encoding="utf-8", errors="replace") as f:
                image = f.read().strip() or None
        except OSError:
            image = None
    return _Process(True, started, image)


def _posix_process(pid: int) -> _Process:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return _Process(False)
    except (PermissionError, OSError):
        pass  # it exists (another user's)
    if os.path.isdir("/proc/self"):
        return _linux_process(pid)
    started = image = None
    try:  # macOS and the BSDs: ps prints the start in UTC here (TZ=UTC0)
        import calendar
        import subprocess
        import time
        out = subprocess.run(["ps", "-o", "lstart=", "-o", "comm=", "-p", str(pid)], capture_output=True,
                             text=True, timeout=5, env={**os.environ, "LC_ALL": "C", "TZ": "UTC0"}).stdout.strip()
        if out:
            parts = out.split()
            started = float(calendar.timegm(time.strptime(" ".join(parts[:5]), "%a %b %d %H:%M:%S %Y")))
            image = os.path.basename(" ".join(parts[5:])) or None
    except Exception:  # noqa: BLE001 - no ps, an odd format, a timeout: cannot be told
        started = image = None
    return _Process(True, started, image)


def _process(pid: object) -> _Process:
    if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0:
        return _Process(None)
    if sys.platform == "win32":
        return _windows_process(pid)
    return _posix_process(pid)


def pid_alive(pid: object) -> bool:
    """Whether a process with this id runs on this machine. True when it cannot be told (no
    usable pid, or no permission to look), so only a file that is surely stale is passed over."""
    return _process(pid).alive is not False


def _started_at(data: dict) -> float | None:
    value = data.get("started_at")
    if not isinstance(value, str) or not value.strip():
        return None
    from datetime import datetime, timezone
    text = value.strip()
    if text.endswith(("Z", "z")):
        text = text[:-1] + "+00:00"
    try:
        moment = datetime.fromisoformat(text)
    except ValueError:
        return None
    if moment.tzinfo is None:
        moment = moment.replace(tzinfo=timezone.utc)  # LibreCAD writes UTC
    return moment.timestamp()


def _looks_like_librecad(image: str) -> bool:
    name = image.replace("\\", "/").rsplit("/", 1)[-1].lower()
    return name.startswith("librecad")


def stale_reason(data: dict) -> str | None:
    """Why the discovery file cannot be trusted, or None when it can. A file is stale when the
    LibreCAD that wrote it no longer runs: it was killed or crashed and left the file behind. Its
    pid may then belong to another program already (process ids are reused), so the process must
    also be the one that wrote the file: a LibreCAD program (when the file says app "LibreCAD") that
    started before the file's started_at. Its token must not be sent anywhere else, since another
    program may now listen on that port (and LibreCAD keeps the token for its next start)."""
    pid = data.get("pid")
    proc = _process(pid)
    if proc.alive is False:
        return f"process {pid} is not running"
    if proc.alive is None and isinstance(pid, int) and not isinstance(pid, bool) and pid > 0 and sys.platform == "win32":
        # it exists, but neither it nor the system's process list can be read: it cannot be the
        # LibreCAD of this user, whose processes this user can always look at
        return f"process {pid} cannot be checked (not a process of this user)"
    if proc.image and data.get("app") == "LibreCAD" and not _looks_like_librecad(proc.image):
        return f"process {pid} is {proc.image}, not LibreCAD (the pid was reused)"
    started_at = _started_at(data)
    if proc.started is not None and started_at is not None and proc.started > started_at + _START_SLACK_S:
        return f"process {pid} started after the file was written (the pid was reused)"
    return None


def is_stale(data: dict) -> bool:
    """True when the discovery file was not written by a LibreCAD that still runs (stale_reason)."""
    return stale_reason(data) is not None


def read_discovery(path: str | os.PathLike) -> dict | None:
    """The discovery file's content, checked, or None when there is no file at ``path``.

    Raises ValueError (naming the file, never the token) when the file exists but cannot be
    used: unreadable, not JSON, or without a usable ``url``/``port`` and ``token``. The returned
    dict always has ``url`` (no trailing slash) and ``token``; other keys are passed through.
    """
    p = Path(path)
    try:
        raw = p.read_text(encoding="utf-8-sig")
    except FileNotFoundError:
        return None
    except IsADirectoryError:
        raise ValueError(f"discovery file {p} is a folder") from None
    except OSError as e:
        raise ValueError(f"discovery file {p} cannot be read: {e.strerror or e}") from e
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as e:
        raise ValueError(f"discovery file {p} is not valid JSON ({e.msg} at line {e.lineno})") from e
    if not isinstance(data, dict):
        raise ValueError(f"discovery file {p} does not hold a JSON object")

    token = data.get("token")
    if not isinstance(token, str) or not token.strip() or token.strip().startswith("${"):
        raise ValueError(f"discovery file {p} has no token")
    url = data.get("url")
    port = data.get("port")
    if isinstance(url, str) and url.strip() and not url.strip().startswith("${"):
        url = url.strip().rstrip("/")
        if not url.startswith(("http://", "https://")):
            raise ValueError(f"discovery file {p} has an unusable url (expected http://host:port)")
    elif isinstance(port, int) and not isinstance(port, bool) and 0 < port < 65536:
        url = f"http://127.0.0.1:{port}"
    else:
        raise ValueError(f"discovery file {p} has neither a url nor a port")
    out = dict(data)
    out["url"] = url
    out["token"] = token.strip()
    return out
