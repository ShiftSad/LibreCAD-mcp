#!/usr/bin/env python3
# This file is part of LibreCAD-mcp, an unofficial fork of LibreCAD.
#
# Copyright (C) 2026 LibreCAD-mcp contributors
#
# This program is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation; either version 2 of the License, or (at your option) any later
# version. See the LICENSE file at the top of the repository.
"""End-to-end smoke test of the LibreCAD-mcp Remote API (docs/remote-api/DESIGN.md, 5.3).

Standard library only. Typical runs, from the repository root:

  py -3 tools/remote-api/e2e_smoke.py --exe <build>/LibreCAD.exe --qt-bin <Qt>/bin
  python3 tools/remote-api/e2e_smoke.py --exe <build>/librecad
  py -3 tools/remote-api/e2e_smoke.py --attach          # a LibreCAD that is already running
  py -3 tools/remote-api/e2e_smoke.py --exe ... --via-mcp
  py -3 tools/remote-api/e2e_smoke.py --selftest        # checks this script, needs no LibreCAD

Without --attach, the script starts its own LibreCAD with a throw-away settings
directory (LIBRECAD_SETTINGS_DIR) that enables the Remote API on a free port, and stops
it at the end. Every check prints PASS or FAIL. The exit code is 0 when every check
passed and 1 when one failed or LibreCAD could not be reached.
"""

from __future__ import annotations

import argparse
import base64
import contextlib
import http.client
import json
import math
import os
import queue
import secrets
import shlex
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import urllib.parse
import zlib
from pathlib import Path
from typing import Any, Callable, Iterable, Iterator

API = "/api/v1"
SETTINGS_ORG = "LibreCAD"
SETTINGS_APP = "LibreCAD-2"
DISCOVERY_NAME = "remote-api.json"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

DEFAULT_TIMEOUT = 40.0   # route default is 10 s on the app side
LONG_TIMEOUT = 135.0     # open, save, save_as, export, new: 120 s on the app side
START_TIMEOUT = 60.0

REPO_ROOT = Path(__file__).resolve().parents[2]

# Section 2.4: every route of API v1 (method, path pattern as /routes reports it).
EXPECTED_ROUTES = [
    ("GET", "/api/v1/status"), ("GET", "/api/v1/routes"), ("GET", "/api/v1/formats"),
    ("GET", "/api/v1/documents"), ("POST", "/api/v1/documents/new"),
    ("POST", "/api/v1/documents/open"), ("POST", "/api/v1/documents/activate"),
    ("POST", "/api/v1/documents/save"), ("POST", "/api/v1/documents/save_as"),
    ("POST", "/api/v1/documents/export"), ("POST", "/api/v1/documents/close"),
    ("GET", "/api/v1/drawing"), ("GET", "/api/v1/render"), ("GET", "/api/v1/view"),
    ("POST", "/api/v1/view"), ("POST", "/api/v1/undo"), ("POST", "/api/v1/redo"),
    ("POST", "/api/v1/command"), ("GET", "/api/v1/commands"),
    ("GET", "/api/v1/layers"), ("POST", "/api/v1/layers"), ("PATCH", "/api/v1/layers/{name}"),
    ("POST", "/api/v1/layers/{name}/current"), ("DELETE", "/api/v1/layers/{name}"),
    ("GET", "/api/v1/blocks"), ("POST", "/api/v1/blocks/{name}/insert"),
    ("GET", "/api/v1/entities"), ("GET", "/api/v1/entities/{id}"), ("POST", "/api/v1/entities"),
    ("PATCH", "/api/v1/entities/{id}"), ("DELETE", "/api/v1/entities/{id}"),
    ("POST", "/api/v1/entities/delete"), ("POST", "/api/v1/entities/transform"),
    ("GET", "/api/v1/selection"), ("POST", "/api/v1/selection"), ("POST", "/api/v1/measure"),
]

# Section 2.4: capabilities reported by /status ("export_dwg" only with DWG support).
EXPECTED_CAPABILITIES = [
    "status", "routes", "documents", "export", "export_pdf", "drawing", "render", "view",
    "undo", "command", "layers", "blocks", "entities", "transform", "selection", "measure",
]

# Section 4.4: the tools of the librecad-mcp server.
EXPECTED_MCP_TOOLS = [
    "get_status", "list_formats", "list_documents", "new_document", "open_document",
    "activate_document", "save_document", "save_document_as", "export_document",
    "close_document", "get_drawing_info", "render_view", "get_view", "set_view", "list_layers",
    "create_layer", "update_layer", "set_current_layer", "delete_layer", "list_blocks",
    "insert_block", "query_entities", "get_entity", "add_entities", "update_entity",
    "delete_entities", "transform_entities", "select_entities", "measure", "undo", "redo",
    "run_command", "list_commands",
]

UNKNOWN_ENTITY_ID = 9007199254740000   # far above any lineage counter of a fresh session
UNKNOWN_DOCUMENT_ID = 987654


# --------------------------------------------------------------------------------------
# Small helpers


def approx(value: Any, expected: float, tol: float = 1e-6) -> bool:
    """True when value is a number within tol (relative, at least absolute) of expected."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return False
    return math.isfinite(value) and abs(value - expected) <= tol * max(1.0, abs(expected))


def approx_point(value: Any, expected: Iterable[float], tol: float = 1e-6) -> bool:
    exp = list(expected)
    return (isinstance(value, list) and len(value) >= 2
            and all(approx(v, e, tol) for v, e in zip(value[:2], exp)))


def same_path(a: Any, b: Any) -> bool:
    if not isinstance(a, str) or not isinstance(b, str) or not a or not b:
        return False
    if os.path.normcase(os.path.normpath(a)) == os.path.normcase(os.path.normpath(b)):
        return True
    try:
        return os.path.samefile(a, b)
    except OSError:
        return False


def short(value: Any, limit: int = 400) -> str:
    text = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False, default=str)
    return text if len(text) <= limit else text[:limit] + "..."


def png_size(data: bytes) -> tuple[int, int] | None:
    """(width, height) from a PNG's IHDR chunk, or None if data is not a PNG."""
    if len(data) < 24 or not data.startswith(PNG_SIGNATURE) or data[12:16] != b"IHDR":
        return None
    return struct.unpack(">II", data[16:24])


def is_pdf(data: bytes) -> bool:
    return data.startswith(b"%PDF")


def is_svg(data: bytes) -> bool:
    return b"<svg" in data[:4096]


def dwg_version(data: bytes) -> str:
    """The version string at offset 0 of a DWG file ("AC1032" for 2018-2026)."""
    return data[:6].decode("ascii", "replace")


def looks_like_dxf(data: bytes) -> bool:
    if data.startswith(b"AutoCAD Binary DXF"):
        return True
    return b"SECTION" in data[:256] and b"EOF" in data[-64:]


def parse_window(header: str | None) -> list[float] | None:
    """The X-LibreCAD-Window header, "minx,miny,maxx,maxy", as four floats."""
    if not header:
        return None
    try:
        values = [float(v) for v in header.split(",")]
    except ValueError:
        return None
    if len(values) != 4 or not all(math.isfinite(v) for v in values):
        return None
    return values


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def env_value(name: str) -> str | None:
    """An environment value, treating "" and unsubstituted "${...}" as unset (as the MCP does)."""
    value = os.environ.get(name, "").strip()
    return None if not value or value.startswith("${") else value


def default_discovery_path() -> Path:
    """Where LibreCAD writes remote-api.json without LIBRECAD_SETTINGS_DIR (DESIGN.md 1.4)."""
    if sys.platform == "win32":
        base = Path(os.environ.get("APPDATA") or Path.home() / "AppData" / "Roaming")
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Application Support"
    else:
        base = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share")
    return base / SETTINGS_ORG / SETTINGS_APP / DISCOVERY_NAME


def read_discovery(path: Path) -> dict | None:
    """The parsed discovery file, or None while it is missing, partial or invalid."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict):
        return None
    if not isinstance(data.get("url"), str) or not isinstance(data.get("token"), str):
        return None
    return data


def pid_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    if sys.platform == "win32":
        import ctypes
        kernel32 = ctypes.WinDLL("kernel32")
        kernel32.OpenProcess.restype = ctypes.c_void_p
        kernel32.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
        kernel32.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
        kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel32.OpenProcess(0x1000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return False
        try:
            code = ctypes.c_ulong()
            if not kernel32.GetExitCodeProcess(handle, ctypes.byref(code)):
                return False
            return code.value == 259                          # STILL_ACTIVE
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def tail(path: Path, lines: int = 40) -> str:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    return "\n".join(text.splitlines()[-lines:])


# --------------------------------------------------------------------------------------
# HTTP client


class Response:
    def __init__(self, method: str, target: str, status: int, reason: str,
                 headers: dict[str, str], body: bytes):
        self.method, self.target = method, target
        self.status, self.reason = status, reason
        self.headers = headers          # lower-case names
        self.body = body
        self._json: Any = None
        self._parsed = False

    @property
    def json(self) -> Any:
        if not self._parsed:
            self._parsed = True
            try:
                self._json = json.loads(self.body.decode("utf-8")) if self.body else None
            except (ValueError, UnicodeDecodeError):
                self._json = None
        return self._json

    @property
    def obj(self) -> dict:
        data = self.json
        return data if isinstance(data, dict) else {}

    @property
    def error(self) -> Any:
        return self.obj.get("error")

    def header(self, name: str) -> str | None:
        return self.headers.get(name.lower())

    def describe(self, limit: int = 400) -> str:
        if self.json is not None:
            body = short(self.json, limit)
        elif self.body[:8] == PNG_SIGNATURE:
            body = f"<png {len(self.body)} bytes>"
        else:
            body = short(self.body.decode("utf-8", "replace"), limit)
        return f"{self.method} {self.target} -> {self.status} {body}"


_DEFAULT = object()
_NO_BODY = object()


class ApiClient:
    """A tiny client of the Remote API. Every JSON answer is also screened for
    skipped_dialogs (DESIGN.md 5.3 step 13), ui_timeout and a leaked token."""

    def __init__(self, url: str, token: str | None, timeout: float = DEFAULT_TIMEOUT):
        parts = urllib.parse.urlsplit(url)
        if parts.scheme != "http" or not parts.hostname:
            raise ValueError(f"not an http URL: {url!r}")
        self.url = url.rstrip("/")
        self.host = parts.hostname
        self.port = parts.port or 80
        self.token = token
        self.timeout = timeout
        self.dialogs: list[tuple[str, Any]] = []
        self.timeouts: list[str] = []
        self.leaks: list[str] = []
        self.requests = 0

    @staticmethod
    def target(path: str, query: Any = None) -> str:
        """A request target: "status" means /api/v1/status. List values repeat the key."""
        if not path.startswith("/"):
            path = f"{API}/{path}"
        if query:
            pairs = query.items() if isinstance(query, dict) else query
            items: list[tuple[str, str]] = []
            for key, value in pairs:
                if value is None:
                    continue
                values = value if isinstance(value, (list, tuple)) else [value]
                for item in values:
                    items.append((key, ("true" if item else "false") if isinstance(item, bool) else str(item)))
            if items:
                path += "?" + urllib.parse.urlencode(items, quote_via=urllib.parse.quote)
        return path

    @staticmethod
    def segment(value: Any) -> str:
        """A path parameter, percent-encoded like the MCP client does (quote(x, safe=''))."""
        return urllib.parse.quote(str(value), safe="")

    def request(self, method: str, path: str, query: Any = None, body: Any = _NO_BODY, *,
                raw_body: bytes | None = None, token: Any = _DEFAULT, bearer: bool = False,
                headers: dict[str, str] | None = None, host: str | None = None,
                timeout: float | None = None, track: bool = True) -> Response:
        target = self.target(path, query)
        data: bytes | None = None
        send: list[tuple[str, str]] = []
        if body is not _NO_BODY:
            data = json.dumps(body).encode("utf-8")
            send.append(("Content-Type", "application/json"))
        elif raw_body is not None:
            data = raw_body
        tok = self.token if token is _DEFAULT else token
        if tok:
            send.append(("Authorization", f"Bearer {tok}") if bearer else ("X-Api-Token", tok))
        for key, value in (headers or {}).items():
            send.append((key, value))
        conn = http.client.HTTPConnection(self.host, self.port, timeout=timeout or self.timeout)
        try:
            conn.putrequest(method, target, skip_host=host is not None, skip_accept_encoding=True)
            if host is not None:
                conn.putheader("Host", host)
            for key, value in send:
                conn.putheader(key, value)
            if data is not None or method in ("POST", "PATCH", "PUT"):
                conn.putheader("Content-Length", str(len(data or b"")))
            conn.endheaders(data)
            resp = conn.getresponse()
            payload = resp.read()
            result = Response(method, target, resp.status, resp.reason,
                              {k.lower(): v for k, v in resp.getheaders()}, payload)
        finally:
            conn.close()
        self.requests += 1
        if track:
            self._screen(result)
        return result

    def _screen(self, r: Response) -> None:
        where = f"{r.method} {r.target}"
        if isinstance(r.json, dict):
            if "skipped_dialogs" in r.json:
                self.dialogs.append((where, r.json["skipped_dialogs"]))
            if r.json.get("error") == "ui_timeout":
                self.timeouts.append(where)
        if self.token and self.token.encode("ascii", "ignore") in r.body:
            self.leaks.append(where)

    def get(self, path: str, query: Any = None, **kw: Any) -> Response:
        return self.request("GET", path, query, **kw)

    def post(self, path: str, body: Any = None, query: Any = None, **kw: Any) -> Response:
        return self.request("POST", path, query, {} if body is None else body, **kw)

    def patch(self, path: str, body: Any, query: Any = None, **kw: Any) -> Response:
        return self.request("PATCH", path, query, body, **kw)

    def delete(self, path: str, query: Any = None, **kw: Any) -> Response:
        return self.request("DELETE", path, query, **kw)


# --------------------------------------------------------------------------------------
# Check bookkeeping


class StepFailed(Exception):
    """Raised by Runner.require: the step cannot go on (the FAIL line is already printed)."""


class Runner:
    def __init__(self, verbose: bool = False):
        self.verbose = verbose
        self.passed = 0
        self.failed: list[str] = []
        self.skipped: list[str] = []
        self.section = ""

    def check(self, name: str, ok: Any, detail: Any = "") -> bool:
        ok = bool(ok)
        label = f"{self.section}: {name}" if self.section else name
        if ok:
            self.passed += 1
        else:
            self.failed.append(label)
        text = detail.describe() if isinstance(detail, Response) else str(detail or "")
        line = ("PASS  " if ok else "FAIL  ") + name
        if text and (not ok or self.verbose):
            line += f"  | {text}"
        print(line, flush=True)
        return ok

    def require(self, name: str, ok: Any, detail: Any = "") -> None:
        if not self.check(name, ok, detail):
            raise StepFailed(name)

    def expect(self, name: str, resp: Response, status: int, error: str | None = None) -> bool:
        ok = resp.status == status and (error is None or resp.error == error)
        return self.check(name, ok, resp)

    def skip(self, name: str, why: str) -> None:
        label = f"{self.section}: {name}" if self.section else name
        self.skipped.append(label)
        print(f"SKIP  {name}  | {why}", flush=True)

    @contextlib.contextmanager
    def step(self, title: str) -> Iterator[None]:
        self.section = title
        print(f"\n== {title}", flush=True)
        try:
            yield
        except StepFailed:
            pass
        except Exception as exc:  # noqa: BLE001 - a broken step must not stop the others
            where = traceback.extract_tb(exc.__traceback__)[-1]
            self.check("step ran to the end", False,
                       f"{type(exc).__name__}: {exc} (line {where.lineno})")
        finally:
            self.section = ""

    def summary(self) -> int:
        print(f"\n{self.passed} passed, {len(self.failed)} failed, {len(self.skipped)} skipped")
        for name in self.failed:
            print(f"  FAILED: {name}")
        return 0 if not self.failed else 1


# --------------------------------------------------------------------------------------
# Starting LibreCAD


def settings_ini(port: int, token: str, paths: dict[str, str] | None = None) -> str:
    """The settings LibreCAD starts with: no first-start dialog, no splash, no update
    check, and the Remote API enabled (DESIGN.md 5.3 step 1). `paths` becomes the
    [Paths] section (see sandbox_paths)."""
    text = (
        "[Startup]\n"
        "FirstLoad=false\n"
        "ShowSplash=false\n"
        "CheckForNewVersions=false\n"
        "\n"
        "[RemoteAPI]\n"
        "Enabled=true\n"
        f"Port={port}\n"
        f"Token={token}\n"
        "Notify=true\n"
        "AllowCommands=true\n"
        "AllowNetworkPaths=false\n"
    )
    if paths:
        text += "\n[Paths]\n" + "".join(f"{key}={value}\n" for key, value in paths.items())
    return text


def deployed_support_dir(exe: Path, sub: str) -> Path | None:
    """The folder a deployed LibreCAD loads `sub` (fonts, patterns) from, if there is one
    (the places RS_System::getDirectoryList looks next to the executable)."""
    base = exe.parent
    candidates = [base / "resources" / sub, base.parent / "Resources" / sub]
    for name in ("librecad", "LibreCAD"):
        candidates += [base.parent / "share" / name / sub, base.parent / "share" / "doc" / name / sub,
                       base.parent / "lib" / name / sub, base.parent / "lib64" / name / sub]
    return next((c for c in candidates if c.is_dir()), None)


def sandbox_paths(exe: Path, settings_dir: Path) -> dict[str, str]:
    """[Paths] for a sandboxed run.

    OtherSettingsDir keeps the files LibreCAD writes next to its settings (command
    aliases, workspaces, pen palettes, shortcuts) in the sandbox instead of the user's
    application data folder. A development build has no fonts or hatch patterns next to
    it, so they come from this checkout: text needs the font "standard" (DESIGN.md 6,
    risk 6; without it every text spec is 422 invalid_entity)."""
    paths = {"OtherSettingsDir": settings_dir.as_posix()}
    for key, sub in (("Fonts", "fonts"), ("Patterns", "patterns")):
        source = REPO_ROOT / "librecad" / "support" / sub
        if deployed_support_dir(exe, sub) is None and source.is_dir():
            paths[key] = source.as_posix()
    return paths


def offscreen_plugin_available(exe: Path) -> bool:
    """False only when a deployed package visibly lacks the qoffscreen platform plugin.
    A development build has no platforms folder and loads plugins from its Qt install."""
    for folder in (exe.parent / "platforms", exe.parent.parent / "PlugIns" / "platforms"):
        if folder.is_dir():
            return any(p.name.lower().startswith(("qoffscreen", "libqoffscreen"))
                       for p in folder.iterdir())
    return True


class LaunchedApp:
    def __init__(self, exe: Path, workdir: Path, port: int, token: str, visible: bool,
                 qt_bin: str | None):
        self.exe = exe
        self.workdir = workdir
        self.port = port
        self.token = token
        self.visible = visible
        self.qt_bin = qt_bin
        self.settings_dir = workdir / "settings"
        self.discovery = self.settings_dir / DISCOVERY_NAME
        self.log_path = workdir / "librecad.log"
        self.proc: subprocess.Popen | None = None
        self._log = None

    def write_settings(self) -> Path:
        ini = self.settings_dir / SETTINGS_ORG / f"{SETTINGS_APP}.ini"
        ini.parent.mkdir(parents=True, exist_ok=True)
        ini.write_text(settings_ini(self.port, self.token, sandbox_paths(self.exe, self.settings_dir)),
                       encoding="utf-8")
        return ini

    def start(self) -> None:
        env = dict(os.environ)
        for key in list(env):
            if key.startswith("LIBRECAD_API_") or key.startswith("LIBRECAD_MCP_"):
                del env[key]
        env["LIBRECAD_SETTINGS_DIR"] = str(self.settings_dir)
        if self.visible:
            env.pop("QT_QPA_PLATFORM", None)
        else:
            env["QT_QPA_PLATFORM"] = "offscreen"
        if self.qt_bin:
            env["PATH"] = self.qt_bin + os.pathsep + env.get("PATH", "")
        self._log = open(self.log_path, "wb")
        self.proc = subprocess.Popen([str(self.exe)], env=env, cwd=str(self.workdir),
                                     stdin=subprocess.DEVNULL, stdout=self._log,
                                     stderr=subprocess.STDOUT)

    def wait_ready(self, timeout: float) -> tuple[ApiClient | None, dict | None, str]:
        """Poll the discovery file, then /status, until both answer or timeout runs out."""
        deadline = time.monotonic() + timeout
        last = "no discovery file yet"
        while time.monotonic() < deadline:
            if self.proc is not None and self.proc.poll() is not None:
                return None, None, f"LibreCAD exited with code {self.proc.returncode} before the API answered"
            info = read_discovery(self.discovery)
            if info is not None:
                try:
                    client = ApiClient(info["url"], info["token"])
                    resp = client.get("status", timeout=10, track=False)
                    if resp.status == 200:
                        return client, info, ""
                    last = f"/status answered {resp.describe()}"
                    if resp.status == 401:
                        break   # the token is wrong: waiting will not fix it
                except (OSError, ValueError, http.client.HTTPException) as exc:
                    last = f"/status not reachable yet: {exc}"
            time.sleep(0.5)
        return None, None, last

    def stop(self) -> None:
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(15)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(15)
        if self._log is not None:
            self._log.close()
            self._log = None


# --------------------------------------------------------------------------------------
# A minimal MCP client over stdio (newline-delimited JSON-RPC 2.0), standard library only


class McpError(Exception):
    pass


class McpStdioClient:
    PROTOCOL_VERSION = "2025-06-18"

    def __init__(self, argv: list[str] | str, env: dict[str, str], cwd: str | None = None,
                 stderr_path: Path | None = None):
        self._stderr = open(stderr_path, "wb") if stderr_path else subprocess.DEVNULL
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self._stderr, env=env, cwd=cwd)
        self._messages: queue.Queue = queue.Queue()
        self._next_id = 0
        self.noise: list[bytes] = []          # stdout lines that are not JSON-RPC
        self.server_requests: list[str] = []
        self.server_info: dict = {}
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                message = json.loads(line)
            except ValueError:
                self._messages.put({"_noise": line})
                continue
            self._messages.put(message if isinstance(message, dict) else {"_noise": line})
        self._messages.put(None)

    def _send(self, message: dict) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write((json.dumps(message) + "\n").encode("utf-8"))
        self.proc.stdin.flush()

    def notify(self, method: str, params: dict | None = None) -> None:
        message: dict[str, Any] = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        self._send(message)

    def request(self, method: str, params: dict | None = None, timeout: float = 60.0) -> dict:
        self._next_id += 1
        rid = self._next_id
        message: dict[str, Any] = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            message["params"] = params
        self._send(message)
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"MCP {method}: no answer within {timeout:.0f} s")
            try:
                incoming = self._messages.get(timeout=min(remaining, 0.5))
            except queue.Empty:
                continue
            if incoming is None:
                raise McpError(f"MCP server closed its output (exit code {self.proc.poll()})")
            if "_noise" in incoming:
                self.noise.append(incoming["_noise"])
                continue
            if "method" in incoming:
                if "id" in incoming:              # a request from the server
                    self._answer(incoming)
                continue                          # a notification
            if incoming.get("id") == rid:
                if "error" in incoming:
                    raise McpError(f"{method}: {short(incoming['error'])}")
                result = incoming.get("result")
                return result if isinstance(result, dict) else {}
            # else: a late answer to an earlier request that timed out

    def _answer(self, request: dict) -> None:
        self.server_requests.append(str(request.get("method")))
        if request.get("method") == "ping":
            self._send({"jsonrpc": "2.0", "id": request["id"], "result": {}})
        else:
            self._send({"jsonrpc": "2.0", "id": request["id"],
                        "error": {"code": -32601, "message": "not supported by e2e_smoke.py"}})

    def initialize(self, timeout: float = 180.0) -> dict:
        result = self.request("initialize", {
            "protocolVersion": self.PROTOCOL_VERSION,
            "capabilities": {},
            "clientInfo": {"name": "librecad-e2e-smoke", "version": "1"},
        }, timeout=timeout)
        self.server_info = result
        self.notify("notifications/initialized")
        return result

    def list_tools(self) -> list[dict]:
        tools: list[dict] = []
        cursor = None
        for _ in range(50):
            result = self.request("tools/list", {"cursor": cursor} if cursor else {})
            tools.extend(t for t in result.get("tools", []) if isinstance(t, dict))
            cursor = result.get("nextCursor")
            if not cursor:
                break
        return tools

    def call_tool(self, name: str, arguments: dict | None = None, timeout: float = 90.0) -> dict:
        return self.request("tools/call", {"name": name, "arguments": arguments or {}}, timeout=timeout)

    def close(self) -> None:
        with contextlib.suppress(OSError):
            if self.proc.stdin is not None:
                self.proc.stdin.close()
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(10)
        if self._stderr is not subprocess.DEVNULL:
            self._stderr.close()


def tool_body(result: dict) -> Any:
    """The JSON a tool returned: structuredContent if present, else the first text item."""
    structured = result.get("structuredContent")
    if isinstance(structured, dict):
        if set(structured) == {"result"}:
            return structured["result"]
        return structured
    for item in result.get("content", []):
        if isinstance(item, dict) and item.get("type") == "text":
            try:
                return json.loads(item.get("text", ""))
            except ValueError:
                return {"text": item.get("text", "")}
    return {}


def tool_images(result: dict) -> list[dict]:
    return [c for c in result.get("content", []) if isinstance(c, dict) and c.get("type") == "image"]


def find_uv() -> str | None:
    found = shutil.which("uv")
    if found:
        return found
    for candidate in (Path.home() / ".local" / "bin" / "uv.exe", Path.home() / ".local" / "bin" / "uv",
                      Path.home() / ".cargo" / "bin" / "uv"):
        if candidate.is_file():
            return str(candidate)
    return None


# --------------------------------------------------------------------------------------
# The scenario (DESIGN.md 5.3 step 3)


class State:
    def __init__(self, out_dir: Path):
        self.out_dir = out_dir
        self.status: dict = {}
        self.dwg_support = False
        self.doc: int | None = None          # the new drawing of step 2
        self.doc2: int | None = None         # the DWG (or DXF) reopened in step 10
        self.opened: list[int] = []          # every document this run created or opened
        self.poly: int | None = None
        self.circle: int | None = None
        self.text: int | None = None
        self.dim: int | None = None
        self.hatch: int | None = None
        self.copy: int | None = None
        self.files: dict[str, Path] = {}


def entity_total(api: ApiClient, doc: int) -> int | None:
    resp = api.get("entities", {"doc": doc, "limit": 1})
    total = resp.obj.get("total") if resp.status == 200 else None
    return total if isinstance(total, int) and not isinstance(total, bool) else None


def step_status(r: Runner, api: ApiClient, st: State, discovery: Path | None) -> None:
    with r.step("1. status, routes and capabilities"):
        resp = api.get("status")
        r.require("GET /status -> 200", resp.status == 200, resp)
        s = st.status = resp.obj
        r.check("app is LibreCAD", s.get("app") == "LibreCAD", short(s.get("app")))
        r.check("api_version is 1.0", s.get("api_version") == "1.0", short(s.get("api_version")))
        r.check("mcp_build is a positive integer",
                isinstance(s.get("mcp_build"), int) and s.get("mcp_build") >= 1, short(s.get("mcp_build")))
        r.check("app_version is set", isinstance(s.get("app_version"), str) and s.get("app_version"),
                short(s.get("app_version")))
        st.dwg_support = bool(s.get("dwg_support"))
        caps = s.get("capabilities") if isinstance(s.get("capabilities"), list) else []
        wanted = EXPECTED_CAPABILITIES + (["export_dwg"] if st.dwg_support else [])
        missing = [c for c in wanted if c not in caps]
        r.check("capabilities list every v1 group", not missing, f"missing {missing}; got {caps}")
        r.check("commands_allowed is true (AllowCommands=true)", s.get("commands_allowed") is True,
                short(s.get("commands_allowed")))
        gui = s.get("gui") if isinstance(s.get("gui"), dict) else {}
        r.check("no modal dialog is open", gui.get("modal_open") is False, short(gui))
        r.check("documents is a list", isinstance(s.get("documents"), list), short(s.get("documents")))
        if discovery is not None:
            r.check("discovery_file names the discovery file",
                    same_path(s.get("discovery_file"), str(discovery)), short(s.get("discovery_file")))
        else:
            r.check("discovery_file is reported", isinstance(s.get("discovery_file"), str),
                    short(s.get("discovery_file")))
        r.check("/status never carries the token", api.token not in resp.body.decode("utf-8", "replace"))
        ctype = resp.header("Content-Type") or ""
        r.check("JSON content type", ctype.startswith("application/json"), ctype)
        r.check("Cache-Control: no-store", (resp.header("Cache-Control") or "").lower() == "no-store",
                resp.header("Cache-Control"))
        r.check("X-Content-Type-Options: nosniff",
                (resp.header("X-Content-Type-Options") or "").lower() == "nosniff",
                resp.header("X-Content-Type-Options"))
        r.check("Connection: close", (resp.header("Connection") or "").lower() == "close",
                resp.header("Connection"))

        resp = api.get("routes")
        r.require("GET /routes -> 200", resp.status == 200, resp)
        routes = resp.obj.get("routes") if isinstance(resp.obj.get("routes"), list) else []
        have = {(str(x.get("method", "")).upper(), x.get("path")) for x in routes if isinstance(x, dict)}
        missing_routes = [f"{m} {p}" for m, p in EXPECTED_ROUTES if (m, p) not in have]
        r.check(f"/routes lists all {len(EXPECTED_ROUTES)} v1 routes", not missing_routes,
                f"missing {missing_routes}")
        fields = ("method", "path", "capability", "changes_document", "timeout_s", "summary")
        incomplete = [f"{x.get('method')} {x.get('path')}" for x in routes
                      if isinstance(x, dict) and not all(k in x for k in fields)]
        r.check("every route has method/path/capability/changes_document/timeout_s/summary",
                routes and not incomplete, f"incomplete: {incomplete[:5]}")

        resp = api.get("formats")
        if r.expect("GET /formats -> 200", resp, 200):
            save = resp.obj.get("save") if isinstance(resp.obj.get("save"), list) else []
            dwg = next((x for x in save if isinstance(x, dict) and x.get("format") == "dwg"), None)
            if st.dwg_support:
                r.check("/formats offers DWG 2018 with the 2026 alias",
                        dwg is not None and "2018" in (dwg.get("versions") or [])
                        and (dwg.get("aliases") or {}).get("2026") == "2018", short(dwg))
            export = resp.obj.get("export") if isinstance(resp.obj.get("export"), list) else []
            r.check("/formats exports dxf, pdf, png and svg",
                    all(f in export for f in ("dxf", "pdf", "png", "svg")), short(export))


def step_new_document(r: Runner, api: ApiClient, st: State) -> None:
    with r.step("2. new document"):
        resp = api.post("documents/new", {}, timeout=LONG_TIMEOUT)
        r.require("POST /documents/new -> 201", resp.status == 201, resp)
        doc = resp.obj.get("document") if isinstance(resp.obj.get("document"), dict) else {}
        r.require("document.id is an integer", isinstance(doc.get("id"), int), short(doc))
        st.doc = doc["id"]
        st.opened.append(st.doc)
        r.check("new document has no path", doc.get("path") is None, short(doc.get("path")))
        r.check("new document is active", doc.get("active") is True, short(doc))
        r.check("Doc has name, modified and entity_count",
                isinstance(doc.get("name"), str) and isinstance(doc.get("modified"), bool)
                and isinstance(doc.get("entity_count"), int), short(doc))
        resp = api.get("documents")
        ids = [d.get("id") for d in resp.obj.get("documents", []) if isinstance(d, dict)]
        r.check("GET /documents lists it as active",
                resp.status == 200 and st.doc in ids and resp.obj.get("active") == st.doc, resp)


def step_layer(r: Runner, api: ApiClient, st: State) -> None:
    with r.step("3. layer 'walls' (red)"):
        if st.doc is None:
            return r.skip("create layer", "no document")
        q = {"doc": st.doc}
        current = api.get("layers", q).obj.get("current")
        resp = api.post("layers", {"name": "walls", "color": "#ff0000"}, q)
        r.require("POST /layers -> 201", resp.status == 201, resp)
        # the layer list widget activates every new layer of the active drawing; make_current is false
        r.check("a new layer does not become current without make_current",
                resp.obj.get("layer", {}).get("current") is False and api.get("layers", q).obj.get("current") == current,
                short(resp.obj.get("layer")))
        layer = resp.obj.get("layer") if isinstance(resp.obj.get("layer"), dict) else {}
        r.check("layer name is walls", layer.get("name") == "walls", short(layer))
        r.check("layer color is #ff0000", str(layer.get("color", "")).lower() == "#ff0000", short(layer))
        r.check("layer changes are reported as not undoable", resp.obj.get("undoable") is False,
                short(resp.obj.get("undoable")))
        resp = api.post("layers", {"name": "walls"}, q)
        r.expect("the same name again -> 409 layer_exists", resp, 409, "layer_exists")
        resp = api.get("layers", q)
        names = [x.get("name") for x in resp.obj.get("layers", []) if isinstance(x, dict)]
        r.check("GET /layers lists walls and 0", resp.status == 200 and "walls" in names and "0" in names,
                resp)


def step_entities(r: Runner, api: ApiClient, st: State) -> None:
    with r.step("4. entities: polyline, circle, text, dimension, hatch, insert"):
        if st.doc is None:
            return r.skip("create entities", "no document")
        q = {"doc": st.doc}
        base = entity_total(api, st.doc)
        r.require("GET /entities?limit=1 reports a total", base is not None)
        specs = [
            {"type": "polyline", "vertices": [[0, 0], [100, 0], [100, 50], [0, 50]], "closed": True},
            {"type": "circle", "center": [150, 25], "radius": 20},
            {"type": "text", "position": [0, 60], "text": "LibreCAD-mcp e2e", "height": 5},
            {"type": "dimension", "dim_type": "linear", "p1": [0, 0], "p2": [100, 0],
             "line_point": [50, -15], "angle_deg": 0},
        ]
        resp = api.post("entities", {"entities": specs, "layer": "walls"}, q)
        r.require("POST /entities (4 specs) -> 201", resp.status == 201, resp)
        ids = resp.obj.get("ids") if isinstance(resp.obj.get("ids"), list) else []
        created = resp.obj.get("created") if isinstance(resp.obj.get("created"), list) else []
        r.require("4 integer ids, all different",
                  len(ids) == 4 and all(isinstance(i, int) for i in ids) and len(set(ids)) == 4, short(ids))
        st.poly, st.circle, st.text, st.dim = ids
        types = [c.get("type") for c in created if isinstance(c, dict)]
        r.check("created types match the specs", types == ["polyline", "circle", "text", "dimension"],
                short(types))
        r.check("undoable is true", resp.obj.get("undoable") is True, short(resp.obj.get("undoable")))
        if len(created) == 4:
            r.check("ids match created[].id", [c.get("id") for c in created] == ids, short(created))
            r.check("every entity is on layer walls", all(c.get("layer") == "walls" for c in created),
                    short([c.get("layer") for c in created]))
            r.check("polyline is closed with 4 vertices",
                    created[0].get("closed") is True and len(created[0].get("vertices") or []) == 4,
                    short(created[0]))
            r.check("circle radius 20 at [150,25]",
                    approx(created[1].get("radius"), 20) and approx_point(created[1].get("center"), [150, 25]),
                    short(created[1]))
            r.check("text keeps its string", created[2].get("text") == "LibreCAD-mcp e2e", short(created[2]))
            dim = created[3]
            r.check("linear dimension measures 100",
                    dim.get("dim_type") == "linear" and approx(dim.get("measurement"), 100, 1e-4),
                    short(dim))
        r.check("entity total +4", entity_total(api, st.doc) == base + 4)

        resp = api.post("entities", {"entities": [{"type": "hatch", "pattern": "SOLID",
                                                   "boundary_ids": [st.poly]}], "layer": "walls"}, q)
        if r.expect("SOLID hatch with boundary_ids=[polyline] -> 201", resp, 201):
            hatch = (resp.obj.get("created") or [{}])[0]
            st.hatch = (resp.obj.get("ids") or [None])[0]
            r.check("hatch type", hatch.get("type") == "hatch", short(hatch))
            if "area" in hatch:
                r.check("hatch area is 5000", approx(hatch.get("area"), 5000, 1e-4), short(hatch.get("area")))
        r.check("entity total +5", entity_total(api, st.doc) == base + 5)

        before = entity_total(api, st.doc)
        resp = api.post("entities", {"entities": [
            {"type": "line", "start": [0, 0], "end": [10, 0]},
            {"type": "insert", "block": "e2e_no_such_block", "position": [0, 0]},
        ]}, q)
        r.expect("insert of a block that does not exist -> 404 unknown_block", resp, 404, "unknown_block")
        r.check("the batch is atomic: the valid line was not created either",
                entity_total(api, st.doc) == before)
        resp = api.post(f"blocks/{api.segment('e2e_no_such_block')}/insert", {"position": [0, 0]}, q)
        r.expect("POST /blocks/{name}/insert unknown -> 404 unknown_block", resp, 404, "unknown_block")
        resp = api.post("entities", {"entities": [{"type": "circle", "center": [0, 0], "radius": 0}]}, q)
        errors = resp.obj.get("errors") if isinstance(resp.obj.get("errors"), dict) else {}
        r.check("radius 0 -> 422 invalid_entity with errors[\"entities[0].radius\"]",
                resp.status == 422 and resp.error == "invalid_entity" and "entities[0].radius" in errors, resp)


def step_query(r: Runner, api: ApiClient, st: State) -> None:
    with r.step("5. query"):
        if st.doc is None or st.poly is None:
            return r.skip("query", "no entities")
        resp = api.get("entities", {"doc": st.doc, "type": "polyline"})
        r.require("GET /entities?type=polyline -> 200", resp.status == 200, resp)
        found = [e for e in resp.obj.get("entities", []) if isinstance(e, dict)]
        r.check("only polylines", found and all(e.get("type") == "polyline" for e in found), short(found))
        mine = next((e for e in found if e.get("id") == st.poly), None)
        r.check("our polyline is listed", mine is not None, short([e.get("id") for e in found]))
        if mine is not None:
            r.check("summary detail reports vertex_count 4", mine.get("vertex_count") == 4, short(mine))
        r.check("paging fields", all(k in resp.obj for k in ("total", "offset", "limit", "next_offset")),
                short(sorted(resp.obj)))
        resp = api.get(f"entities/{st.poly}", {"doc": st.doc})
        ent = resp.obj.get("entity") if isinstance(resp.obj.get("entity"), dict) else {}
        r.check("GET /entities/{id} -> full polyline",
                resp.status == 200 and ent.get("id") == st.poly and len(ent.get("vertices") or []) == 4
                and ent.get("closed") is True, resp)
        bbox = ent.get("bbox") if isinstance(ent.get("bbox"), dict) else {}
        r.check("bbox is [0,0]..[100,50]",
                approx_point(bbox.get("min"), [0, 0]) and approx_point(bbox.get("max"), [100, 50]), short(bbox))
        resp = api.get("entities", {"doc": st.doc, "layer": "walls", "limit": 1000})
        expected = 5 if st.hatch is not None else 4
        r.check(f"layer=walls finds the {expected} entities", resp.obj.get("total") == expected, resp)


def step_measure(r: Runner, api: ApiClient, st: State) -> None:
    with r.step("6. measure"):
        if st.doc is None or st.poly is None:
            return r.skip("measure", "no polyline")
        q = {"doc": st.doc}
        resp = api.post("measure", {"kind": "area", "ids": [st.poly]}, q)
        r.check("area of the 100x50 polyline is 5000, perimeter 300",
                resp.status == 200 and approx(resp.obj.get("area"), 5000, 1e-6)
                and approx(resp.obj.get("perimeter"), 300, 1e-6), resp)
        resp = api.post("measure", {"kind": "distance", "from": [0, 0], "to": [3, 4]}, q)
        r.check("distance [0,0]..[3,4] is 5", resp.status == 200 and approx(resp.obj.get("distance"), 5), resp)


def step_transform_undo(r: Runner, api: ApiClient, st: State) -> None:
    with r.step("7. transform copy, undo, redo"):
        if st.doc is None or st.circle is None:
            return r.skip("transform", "no circle")
        q = {"doc": st.doc}
        before = entity_total(api, st.doc)
        known = {st.poly, st.circle, st.text, st.dim, st.hatch}
        resp = api.post("entities/transform", {"ids": [st.circle], "op": "move", "offset": [0, 100],
                                               "copy": True}, q)
        r.require("move with copy -> 200", resp.status == 200, resp)
        new_ids = resp.obj.get("ids") if isinstance(resp.obj.get("ids"), list) else []
        r.require("one new id", len(new_ids) == 1 and isinstance(new_ids[0], int) and new_ids[0] not in known,
                  short(new_ids))
        st.copy = new_ids[0]
        r.check("originals_kept and undoable", resp.obj.get("originals_kept") is True
                and resp.obj.get("undoable") is True, resp)
        copies = resp.obj.get("entities") if isinstance(resp.obj.get("entities"), list) else []
        r.check("the copy is at [150,125]", copies and approx_point(copies[0].get("center"), [150, 125]),
                short(copies))
        r.check("entity total +1", entity_total(api, st.doc) == before + 1)
        resp = api.get(f"entities/{st.circle}", q)
        r.check("the original circle did not move",
                approx_point(resp.obj.get("entity", {}).get("center"), [150, 25]), resp)

        resp = api.post("undo", {}, q)
        r.check("POST /undo -> undone 1, can_redo",
                resp.status == 200 and resp.obj.get("undone") == 1 and resp.obj.get("can_redo") is True, resp)
        r.check("entity total is back", entity_total(api, st.doc) == before)
        resp = api.get(f"entities/{st.copy}", q)
        r.expect("the undone copy -> 404 unknown_entity", resp, 404, "unknown_entity")
        resp = api.post("redo", {}, q)
        r.check("POST /redo -> redone 1", resp.status == 200 and resp.obj.get("redone") == 1, resp)
        r.check("entity total +1 again", entity_total(api, st.doc) == before + 1)

        resp = api.post("entities/transform", {"ids": [st.text], "op": "move", "offset": [0, 5]}, q)
        r.check("a plain move keeps the id", resp.status == 200 and resp.obj.get("ids") == [st.text], resp)


def step_render(r: Runner, api: ApiClient, st: State) -> None:
    with r.step("8. render"):
        if st.doc is None:
            return r.skip("render", "no document")
        resp = api.get("render", {"doc": st.doc, "mode": "extents", "width": 640, "height": 480},
                       timeout=LONG_TIMEOUT)
        r.require("GET /render -> 200", resp.status == 200, resp)
        r.check("Content-Type image/png", (resp.header("Content-Type") or "").startswith("image/png"),
                resp.header("Content-Type"))
        r.check("PNG of 640x480", png_size(resp.body) == (640, 480), short(png_size(resp.body)))
        window = parse_window(resp.header("X-LibreCAD-Window"))
        r.check("X-LibreCAD-Window is minx,miny,maxx,maxy",
                window is not None and window[0] < window[2] and window[1] < window[3],
                short(resp.header("X-LibreCAD-Window")))
        if window is not None:
            r.check("the window covers the polyline [0,0]..[100,50]",
                    window[0] <= 0 + 1e-6 and window[1] <= 0 + 1e-6 and window[2] >= 100 - 1e-6
                    and window[3] >= 50 - 1e-6, short(window))
        try:
            scale = float(resp.header("X-LibreCAD-Scale") or "nan")
        except ValueError:
            scale = float("nan")
        r.check("X-LibreCAD-Scale > 0", math.isfinite(scale) and scale > 0, resp.header("X-LibreCAD-Scale"))
        resp = api.get("render", {"doc": st.doc, "mode": "view", "width": 320, "height": 240},
                       timeout=LONG_TIMEOUT)
        r.check("mode=view -> 320x240 PNG", resp.status == 200 and png_size(resp.body) == (320, 240), resp)


def step_files(r: Runner, api: ApiClient, st: State) -> None:
    with r.step("9. save_as DXF; export DWG 2018, PDF, SVG, PNG"):
        if st.doc is None:
            return r.skip("files", "no document")
        q = {"doc": st.doc}
        st.out_dir.mkdir(parents=True, exist_ok=True)
        dxf = st.out_dir / "e2e.dxf"
        resp = api.post("documents/save_as", {"path": str(dxf), "format": "dxf"}, q, timeout=LONG_TIMEOUT)
        if r.expect("save_as DXF -> 200", resp, 200):
            st.files["dxf"] = dxf
            doc = resp.obj.get("document") if isinstance(resp.obj.get("document"), dict) else {}
            r.check("the document now refers to the DXF", same_path(doc.get("path"), str(dxf)), short(doc))
            r.check("and is not modified", doc.get("modified") is False, short(doc))
            r.check("format dxf, left_out is a list",
                    resp.obj.get("format") == "dxf" and isinstance(resp.obj.get("left_out"), list), resp)
            data = dxf.read_bytes() if dxf.is_file() else b""
            r.check("the file is a DXF", looks_like_dxf(data), f"{len(data)} bytes")

        def export(name: str, body: dict, verify: Callable[[bytes], bool], what: str) -> Path | None:
            path = st.out_dir / name
            resp = api.post("documents/export", dict(body, path=str(path)), q, timeout=LONG_TIMEOUT)
            if not r.expect(f"export {name} -> 200", resp, 200):
                return None
            data = path.read_bytes() if path.is_file() else b""
            r.check(f"{name} is {what}", data and verify(data), f"{len(data)} bytes, starts {data[:8]!r}")
            if "bytes" in resp.obj:
                r.check(f"{name}: reported bytes match the file", resp.obj.get("bytes") == len(data),
                        f"reported {resp.obj.get('bytes')}, file {len(data)}")
            return path

        if st.dwg_support:
            st.files["dwg"] = export("e2e_2018.dwg", {"format": "dwg", "version": "2018"},
                                     lambda d: dwg_version(d) == "AC1032", "an AC1032 DWG")
            export("e2e_2026.dwg", {"format": "dwg", "version": "2026"},
                   lambda d: dwg_version(d) == "AC1032", "an AC1032 DWG (alias 2026)")
        else:
            resp = api.post("documents/export", {"path": str(st.out_dir / "x.dwg"), "format": "dwg"}, q)
            r.expect("built without DWG support: export dwg -> 501 dwg_unsupported", resp, 501,
                     "dwg_unsupported")
        st.files["pdf"] = export("e2e.pdf", {"format": "pdf"}, is_pdf, "a PDF")
        st.files["svg"] = export("e2e.svg", {"format": "svg"}, is_svg, "an SVG")
        st.files["png"] = export("e2e.png", {"format": "png", "width": 800, "height": 600},
                                 lambda d: png_size(d) == (800, 600), "an 800x600 PNG")

        resp = api.get("documents")
        doc = next((d for d in resp.obj.get("documents", []) if isinstance(d, dict) and d.get("id") == st.doc), {})
        if "dxf" in st.files:
            r.check("exports did not rename the document", same_path(doc.get("path"), str(dxf)), short(doc))
            r.check("exports left the document unmodified", doc.get("modified") is False, short(doc))


def step_close_open(r: Runner, api: ApiClient, st: State) -> None:
    with r.step("10. close, open the DWG, /drawing"):
        if st.doc is None:
            return r.skip("close/open", "no document")
        resp = api.post("documents/close", {}, {"doc": st.doc})
        if resp.status == 409 and resp.error == "unsaved_changes":
            r.check("close after save -> 200 (the exports left the drawing modified)", False, resp)
            resp = api.post("documents/close", {"discard_changes": True}, {"doc": st.doc})
        else:
            r.check("close -> 200 closed, nothing discarded",
                    resp.status == 200 and resp.obj.get("closed") is True
                    and resp.obj.get("discarded_changes") is False, resp)
        if resp.status == 200 and st.doc in st.opened:
            st.opened.remove(st.doc)
        resp = api.get("documents")
        ids = [d.get("id") for d in resp.obj.get("documents", []) if isinstance(d, dict)]
        r.check("the document is gone from /documents", st.doc not in ids, short(ids))

        source = st.files.get("dwg") or st.files.get("dxf")
        if source is None:
            return r.skip("open", "no saved DWG/DXF from step 9")
        resp = api.post("documents/open", {"path": str(source)}, timeout=LONG_TIMEOUT)
        r.require(f"open {source.name} -> 201", resp.status == 201, resp)
        doc = resp.obj.get("document") if isinstance(resp.obj.get("document"), dict) else {}
        r.require("document.id is an integer", isinstance(doc.get("id"), int), short(doc))
        st.doc2 = doc["id"]
        st.opened.append(st.doc2)
        r.check("warnings is a list", isinstance(resp.obj.get("warnings", []), list), resp)
        resp = api.post("documents/open", {"path": str(source)}, timeout=LONG_TIMEOUT)
        r.check("opening it again -> 200 already_open, same id",
                resp.status == 200 and resp.obj.get("already_open") is True
                and (resp.obj.get("document") or {}).get("id") == st.doc2, resp)

        resp = api.get("drawing", {"doc": st.doc2})
        r.require("GET /drawing -> 200", resp.status == 200, resp)
        fmt = resp.obj.get("format") if isinstance(resp.obj.get("format"), dict) else {}
        if source.suffix.lower() == ".dwg":
            r.check("format is DWG version 2018", fmt.get("format") == "dwg" and fmt.get("version") == "2018",
                    short(fmt))
        else:
            r.check("format is DXF", fmt.get("format") == "dxf", short(fmt))
        counts = resp.obj.get("counts") if isinstance(resp.obj.get("counts"), dict) else {}
        by_type = counts.get("by_type") if isinstance(counts.get("by_type"), dict) else {}
        r.check("the reopened drawing has the polyline, circles, text and dimension",
                all(by_type.get(t, 0) >= n for t, n in (("polyline", 1), ("circle", 2), ("text", 1),
                                                        ("dimension", 1))), short(counts))
        r.check("units and extents are reported",
                isinstance(resp.obj.get("units"), str) and isinstance(resp.obj.get("extents"), dict),
                short({k: resp.obj.get(k) for k in ("units", "extents")}))
        layers = api.get("layers", {"doc": st.doc2})
        names = [x.get("name") for x in layers.obj.get("layers", []) if isinstance(x, dict)]
        r.check("layer walls survived the round trip", "walls" in names, short(names))


def step_command(r: Runner, api: ApiClient, st: State) -> None:
    with r.step("11. command line"):
        if st.doc2 is None:
            return r.skip("command", "no reopened document")
        q = {"doc": st.doc2}
        before = entity_total(api, st.doc2)
        resp = api.post("command", {"command": "line", "inputs": ["0,0", "10,10", "escape", "escape"]}, q,
                        timeout=LONG_TIMEOUT)
        r.require("POST /command line 0,0 10,10 escape escape -> 200", resp.status == 200, resp)
        r.check("output is a non-empty list", isinstance(resp.obj.get("output"), list) and resp.obj["output"],
                short(resp.obj.get("output")))
        r.check("no unknown commands", not resp.obj.get("unknown_commands"), short(resp.obj.get("unknown_commands")))
        r.check("one more entity", before is not None and entity_total(api, st.doc2) == before + 1)
        # one escape ends the segment chain, the second leaves the tool (run_command's docstring)
        r.check("two escapes leave no active command", resp.obj.get("active_action") is None,
                short(resp.obj.get("active_action")))
        if resp.obj.get("active_action"):
            api.post("command", {"command": "escape"}, q)   # leave the drawing idle; not a check
        resp = api.post("command", {"command": ""}, q)
        r.expect("an empty command -> 400 bad_param", resp, 400, "bad_param")
        resp = api.get("commands")
        commands = resp.obj.get("commands") if isinstance(resp.obj.get("commands"), list) else []
        r.check("GET /commands lists commands", resp.status == 200 and len(commands) > 10, resp)


def step_negative(r: Runner, api: ApiClient, st: State) -> None:
    with r.step("12. negative checks"):
        resp = api.get("status", token=None)
        r.check("no token -> 401 unauthorized with app LibreCAD",
                resp.status == 401 and resp.error == "unauthorized" and resp.obj.get("app") == "LibreCAD", resp)
        resp = api.get("status", token="0" * 32)
        r.expect("wrong token -> 401", resp, 401, "unauthorized")
        resp = api.get("status", bearer=True)
        r.expect("Authorization: Bearer works -> 200", resp, 200)
        resp = api.get("nope")
        r.check("unknown route -> bare 404 not_found", resp.status == 404 and resp.error == "not_found", resp)
        resp = api.get("/api/v1/status/")
        r.expect("trailing slash -> 404 not_found", resp, 404, "not_found")
        resp = api.get("statuses")
        r.expect("/statuses does not match /status -> 404", resp, 404, "not_found")
        resp = api.request("DELETE", "status")
        r.expect("wrong method on a known route -> 405 method_not_allowed", resp, 405, "method_not_allowed")
        resp = api.request("OPTIONS", "status")
        r.check("OPTIONS -> 405", resp.status == 405, resp)
        resp = api.get("status", headers={"Origin": "http://example.com"})
        r.expect("an Origin header -> 403 origin_not_allowed", resp, 403, "origin_not_allowed")
        resp = api.get("status", host="evil.example:80")
        r.expect("a foreign Host header -> 421 bad_host", resp, 421, "bad_host")
        resp = api.get("drawing", {"doc": UNKNOWN_DOCUMENT_ID})
        r.expect("unknown document -> 404 unknown_document", resp, 404, "unknown_document")
        resp = api.post("documents/open", {"path": "relative/e2e.dxf"})
        r.expect("relative path -> 400 bad_path", resp, 400, "bad_path")
        resp = api.post("documents/open", {"path": str(st.out_dir / "does-not-exist.dxf")})
        r.expect("missing file -> 404 file_not_found", resp, 404, "file_not_found")
        doc = st.doc2
        if doc is None:
            return r.skip("document checks", "no open document")
        q = {"doc": doc}
        resp = api.get(f"entities/{UNKNOWN_ENTITY_ID}", q)
        r.expect("unknown entity id -> 404 unknown_entity", resp, 404, "unknown_entity")
        target = st.files.get("png")
        if target is not None and target.is_file():
            stamp = (target.stat().st_size, target.stat().st_mtime_ns)
            resp = api.post("documents/export", {"path": str(target), "format": "png"}, q,
                            timeout=LONG_TIMEOUT)
            r.expect("export over an existing file -> 409 file_exists", resp, 409, "file_exists")
            r.check("the existing file was not touched",
                    (target.stat().st_size, target.stat().st_mtime_ns) == stamp)
        else:
            r.skip("file_exists", "no exported PNG from step 9")
        resp = api.post("undo", {"bogus": 1}, q)
        r.check("an unknown body field -> 400 unknown_field",
                resp.status == 400 and resp.error == "unknown_field"
                and resp.obj.get("field", "bogus") == "bogus", resp)
        resp = api.get("drawing", {"doc": doc, "detial": "full"})
        r.expect("an unknown query parameter -> 400 unknown_field", resp, 400, "unknown_field")
        resp = api.get("drawing", [("doc", doc), ("doc", doc)])
        r.expect("doc given twice -> 400 bad_param", resp, 400, "bad_param")
        # a NUL would cut the name the system opens: run.bat would be written as a ".dxf"
        resp = api.post("documents/export", {"path": str(st.out_dir / "run.bat") + "\x00.dxf", "format": "dxf"}, q,
                        timeout=LONG_TIMEOUT)
        r.expect("a path with a NUL -> 400 bad_path", resp, 400, "bad_path")
        r.check("nothing was written", not (st.out_dir / "run.bat").exists())
        # a connection that sends nothing holds up nobody else
        with socket.create_connection((api.host, api.port), timeout=5):
            started = time.monotonic()
            resp = api.get("status")
            took = time.monotonic() - started
        r.check("an idle connection does not hold up /status", resp.status == 200 and took < 3.0,
                f"{took:.2f} s, {resp.status}")
        # a client that ends its sending after the request (ncat, socat) still reads the answer
        raw = b""
        with socket.create_connection((api.host, api.port), timeout=10) as sock:
            sock.sendall((f"GET /api/v1/status HTTP/1.1\r\nHost: 127.0.0.1:{api.port}\r\n"
                          f"X-Api-Token: {api.token}\r\n\r\n").encode())
            sock.shutdown(socket.SHUT_WR)
            try:
                while True:
                    chunk = sock.recv(65536)
                    if not chunk:
                        break
                    raw += chunk
            except OSError as e:
                raw += f"<<{type(e).__name__}>>".encode()
        while raw.startswith(b"HTTP/1.1 1") and b"\r\n\r\n" in raw:  # an interim 100 Continue
            raw = raw.split(b"\r\n\r\n", 1)[1]
        r.check("a client that half-closes after its request gets the answer", raw.startswith(b"HTTP/1.1 200"),
                short(raw[:60]))
        # small requests are answered at once (a reader that slept between looks took 50 ms each)
        times = []
        for _ in range(11):
            started = time.monotonic()
            api.get("status")
            times.append(time.monotonic() - started)
        times.sort()
        r.check("small requests are answered at once (median under 40 ms)", times[5] < 0.04,
                f"{times[5] * 1000:.1f} ms")


def step_mcp(r: Runner, api: ApiClient, st: State, args: argparse.Namespace, workdir: Path,
             discovery: Path | None) -> None:
    with r.step("MCP (--via-mcp): librecad-mcp over stdio"):
        argv: list[str] | str
        if args.mcp_command:
            # Windows: hand the command line to CreateProcess as it is (shlex would keep quotes)
            argv = args.mcp_command if sys.platform == "win32" else shlex.split(args.mcp_command)
        else:
            uv = find_uv()
            if uv is None:
                return r.check("uv is installed (or pass --mcp-command)", False)
            argv = [uv, "run", "--project", str(Path(args.mcp_project).resolve()), "librecad-mcp"]
        env = {k: v for k, v in os.environ.items()
               if not (k.startswith("LIBRECAD_API_") or k.startswith("LIBRECAD_MCP_"))}
        if discovery is not None:
            env["LIBRECAD_API_DISCOVERY"] = str(discovery)
        else:
            env["LIBRECAD_API_URL"], env["LIBRECAD_API_TOKEN"] = api.url, api.token or ""
        print(f"      starting: {argv if isinstance(argv, str) else ' '.join(argv)}", flush=True)
        client = McpStdioClient(argv, env, stderr_path=workdir / "librecad-mcp.stderr.log")
        try:
            info = client.initialize()
            r.require("initialize", isinstance(info.get("protocolVersion"), str), short(info))
            tools = {t.get("name") for t in client.list_tools()}
            missing = [t for t in EXPECTED_MCP_TOOLS if t not in tools]
            r.check(f"tools/list has the {len(EXPECTED_MCP_TOOLS)} tools of DESIGN.md 4.4", not missing,
                    f"missing {missing}")

            result = client.call_tool("get_status")
            body = tool_body(result)
            r.check("get_status reaches LibreCAD", isinstance(body, dict) and body.get("app") == "LibreCAD"
                    and not result.get("isError"), short(result))
            r.check("get_status never shows the token", api.token not in json.dumps(result))

            doc = st.doc2 if st.doc2 is not None else st.doc
            if doc is None:
                return r.skip("add_entities/render_view", "no open document")
            result = client.call_tool("add_entities", {
                "entities": [{"type": "line", "start": [0, 0], "end": [25, 25]}], "document_id": doc})
            body = tool_body(result)
            ids = body.get("ids") if isinstance(body, dict) else None
            r.check("add_entities returns one id",
                    isinstance(ids, list) and len(ids) == 1 and not body.get("error"), short(result))
            if isinstance(ids, list) and ids:
                resp = api.get(f"entities/{ids[0]}", {"doc": doc})
                r.check("the line is in the drawing", resp.status == 200
                        and (resp.obj.get("entity") or {}).get("type") == "line", resp)

            result = client.call_tool("render_view", {"document_id": doc, "width_px": 400, "height_px": 300})
            images = tool_images(result)
            data = b""
            if images:
                with contextlib.suppress(ValueError, TypeError):
                    data = base64.b64decode(images[0].get("data", ""), validate=False)
            r.check("render_view returns a PNG image",
                    images and images[0].get("mimeType") == "image/png" and data.startswith(PNG_SIGNATURE),
                    short([{k: v for k, v in c.items() if k != "data"} for c in result.get("content", [])]))
            r.check("stdout carried only JSON-RPC", not client.noise, short([n[:80] for n in client.noise]))
        finally:
            client.close()


# --------------------------------------------------------------------------------------
# Driver


def run_scenario(r: Runner, api: ApiClient, st: State, args: argparse.Namespace, workdir: Path,
                 discovery: Path | None) -> None:
    step_status(r, api, st, discovery)
    step_new_document(r, api, st)
    step_layer(r, api, st)
    step_entities(r, api, st)
    step_query(r, api, st)
    step_measure(r, api, st)
    step_transform_undo(r, api, st)
    step_render(r, api, st)
    step_files(r, api, st)
    step_close_open(r, api, st)
    step_command(r, api, st)
    step_negative(r, api, st)
    if args.via_mcp:
        step_mcp(r, api, st, args, workdir, discovery)
    with r.step("13. no hidden dialog, no timeout, no token in any answer"):
        r.check(f"no answer carried skipped_dialogs ({api.requests} requests)", not api.dialogs,
                short(api.dialogs))
        r.check("no ui_timeout", not api.timeouts, short(api.timeouts))
        r.check("no answer contained the token", not api.leaks, short(api.leaks))


def close_documents(api: ApiClient, st: State) -> None:
    """--attach: close what this run opened, without asking; never touch other drawings."""
    for doc in list(st.opened):
        with contextlib.suppress(OSError, http.client.HTTPException):
            api.post("documents/close", {"discard_changes": True}, {"doc": doc}, track=False)


def attach_client(args: argparse.Namespace) -> tuple[ApiClient | None, Path | None, str]:
    url, token = env_value("LIBRECAD_API_URL"), env_value("LIBRECAD_API_TOKEN")
    if url and token and not args.discovery:
        return ApiClient(url, token), None, "LIBRECAD_API_URL / LIBRECAD_API_TOKEN"
    path = Path(args.discovery or env_value("LIBRECAD_API_DISCOVERY") or default_discovery_path())
    info = read_discovery(path)
    if info is None:
        return None, path, f"no readable discovery file at {path} (is the Remote API enabled?)"
    return ApiClient(url or info["url"], token or info["token"]), path, str(path)


class Session:
    """What run_e2e has to clean up, whatever happened."""

    def __init__(self) -> None:
        self.app: LaunchedApp | None = None
        self.api: ApiClient | None = None


def connect(r: Runner, args: argparse.Namespace, workdir: Path, session: Session) -> Path | None:
    """Step 0: attach, or start LibreCAD. Sets session.api when the API answers; returns
    the discovery file path when one is known."""
    if args.attach:
        with r.step("0. attach to a running LibreCAD"):
            api, discovery, source = attach_client(args)
            r.require(f"configuration from {source}", api is not None)
            resp = api.get("status", timeout=10, track=False)
            r.require("GET /status -> 200", resp.status == 200, resp)
            session.api = api
            return discovery
        return None

    exe = Path(args.exe or env_value("LIBRECAD_EXE") or "")
    visible = args.visible
    with r.step("0. start LibreCAD"):
        r.require(f"executable exists: {exe}", exe.is_file())
        if not visible and not offscreen_plugin_available(exe):
            print("      the qoffscreen platform plugin is missing next to the executable: running visibly",
                  flush=True)
            visible = True
        app = session.app = LaunchedApp(exe.resolve(), workdir, args.port or free_port(),
                                        secrets.token_hex(16), visible, args.qt_bin)
        ini = app.write_settings()
        print(f"      settings: {ini}\n      port {app.port}, "
              f"{'visible' if visible else 'QT_QPA_PLATFORM=offscreen'}, log {app.log_path}", flush=True)
        app.start()
        api, info, why = app.wait_ready(args.start_timeout)
        if api is None:
            detail = why
            log = tail(app.log_path)
            if log:
                detail += "\n--- librecad.log (tail) ---\n" + log
            if app.settings_dir.is_dir():
                detail += "\n--- settings dir ---\n" + "\n".join(
                    str(p.relative_to(app.settings_dir)) for p in app.settings_dir.rglob("*"))
            r.require(f"the Remote API answers within {args.start_timeout:.0f} s", False, detail)
        session.api = api
        r.check("discovery url is http://127.0.0.1:<port>",
                info.get("url") == f"http://127.0.0.1:{app.port}", short(info.get("url")))
        r.check("discovery token is the configured one", info.get("token") == app.token)
        r.check("discovery pid is the started process", info.get("pid") == app.proc.pid,
                f"{info.get('pid')} vs {app.proc.pid}")
        r.check("discovery has port, app, app_version, mcp_build, api_version, started_at",
                all(k in info for k in ("port", "app", "app_version", "mcp_build", "api_version",
                                        "started_at")), short(sorted(info)))
    return session.app.discovery if session.app is not None else None


def stop_app(r: Runner, app: LaunchedApp) -> None:
    """Step 14 (DESIGN.md 5.3 step 5): terminate, then the discovery file must be gone or
    stale. proc.terminate() gives the app no chance to remove it on Windows."""
    with r.step("14. stop LibreCAD"):
        app.stop()
        info = read_discovery(app.discovery)
        pid = app.proc.pid if app.proc else -1
        if info is None:
            r.check("the discovery file is gone", True)
        else:
            r.check("the discovery file is stale (its pid is not running) and tolerated",
                    info.get("pid") == pid and not pid_alive(pid), short(info.get("pid")))
    if r.failed:
        log = tail(app.log_path, 25)
        if log:
            print("--- librecad.log (tail) ---\n" + log)


def run_e2e(args: argparse.Namespace) -> int:
    if not args.attach and not (args.exe or env_value("LIBRECAD_EXE")):
        print("error: pass --exe <LibreCAD executable> (or set LIBRECAD_EXE), or use --attach",
              file=sys.stderr)
        return 2
    r = Runner(args.verbose)
    workdir = Path(tempfile.mkdtemp(prefix="librecad-e2e-")).resolve()
    st = State(workdir / "out")
    session = Session()
    print(f"work dir: {workdir}")
    try:
        discovery = connect(r, args, workdir, session)
        if session.api is not None:
            run_scenario(r, session.api, st, args, workdir, discovery)
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        r.failed.append("interrupted")
    finally:
        if args.attach and session.api is not None:
            close_documents(session.api, st)
        if session.app is not None:
            stop_app(r, session.app)
        if args.keep:
            print(f"kept: {workdir}")
        else:
            shutil.rmtree(workdir, ignore_errors=True)
    return r.summary()


# --------------------------------------------------------------------------------------
# --selftest: exercise this script's helpers against a stub server, no LibreCAD needed


def _tiny_png(width: int, height: int) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    raw = b"".join(b"\x00" + b"\xff" * width * 3 for _ in range(height))
    return (PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def _stub_server(token: str):
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args: Any) -> None:
            pass

        def send(self, status: int, obj: Any = None, raw: bytes | None = None,
                 ctype: str = "application/json; charset=utf-8", extra: Iterable[tuple[str, str]] = ()) -> None:
            body = raw if raw is not None else json.dumps(obj).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Connection", "close")
            for key, value in extra:
                self.send_header(key, value)
            self.end_headers()
            self.wfile.write(body)
            self.close_connection = True

        def handle_any(self) -> None:
            port = self.server.server_address[1]
            length = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(length) if length else b""
            if self.headers.get("Host") not in (f"127.0.0.1:{port}", f"localhost:{port}"):
                return self.send(421, {"error": "bad_host"})
            if self.headers.get("Origin") is not None:
                return self.send(403, {"error": "origin_not_allowed"})
            auth = self.headers.get("Authorization", "")
            given = self.headers.get("X-Api-Token") or (auth[7:] if auth.startswith("Bearer ") else None)
            if given != token:
                return self.send(401, {"error": "unauthorized", "app": "LibreCAD"})
            if self.command == "OPTIONS":
                return self.send(405, {"error": "method_not_allowed", "allowed": ["GET"]})
            split = urllib.parse.urlsplit(self.path)
            segments = [urllib.parse.unquote(s) for s in split.path.split("/")]
            query = urllib.parse.parse_qs(split.query, keep_blank_values=True)
            path = "/".join(segments)
            if path == "/api/v1/status" and self.command == "GET":
                return self.send(200, {"app": "LibreCAD", "api_version": "1.0", "mcp_build": 1})
            if path == "/api/v1/echo":
                return self.send(200, {"method": self.command, "query": query, "raw_query": split.query,
                                       "body": json.loads(body) if body else None})
            if len(segments) == 5 and segments[:4] == ["", "api", "v1", "entities"]:
                return self.send(200, {"param": segments[4]})
            if path == "/api/v1/render":
                w, h = int(query["width"][0]), int(query["height"][0])
                return self.send(200, raw=_tiny_png(w, h), ctype="image/png",
                                 extra=[("X-LibreCAD-Window", "-1.5,-2,101.5,52"), ("X-LibreCAD-Scale", "4.25")])
            if path == "/api/v1/dialog":
                return self.send(200, {"ok": True, "skipped_dialogs": [{"class": "QMessageBox",
                                                                        "title": "t", "text": "x"}]})
            if path == "/api/v1/leak":
                return self.send(200, {"token": token})
            return self.send(404, {"error": "not_found"})

        do_GET = do_POST = do_PATCH = do_DELETE = do_OPTIONS = handle_any

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


_STUB_MCP_SERVER = r'''
import json, sys

def send(msg):
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()

def read():
    line = sys.stdin.readline()
    return json.loads(line) if line.strip() else None

sys.stderr.write("stub MCP server: stderr is not protocol\n")
PNG = sys.argv[1]
while True:
    msg = read()
    if msg is None:
        break
    method, rid = msg.get("method"), msg.get("id")
    if rid is None:
        continue
    if method == "initialize":
        send({"jsonrpc": "2.0", "method": "notifications/message", "params": {"level": "info", "data": "hi"}})
        send({"jsonrpc": "2.0", "id": rid, "result": {"protocolVersion": msg["params"]["protocolVersion"],
              "capabilities": {"tools": {}}, "serverInfo": {"name": "stub", "version": "0"}}})
    elif method == "tools/list":
        if (msg.get("params") or {}).get("cursor") == "p2":
            send({"jsonrpc": "2.0", "id": rid, "result": {"tools": [{"name": "render_view"}]}})
        else:
            send({"jsonrpc": "2.0", "id": rid, "result": {"tools": [{"name": "get_status"}], "nextCursor": "p2"}})
    elif method == "tools/call" and msg["params"]["name"] == "get_status":
        send({"jsonrpc": "2.0", "id": "srv-1", "method": "ping"})
        answer = read()
        ok = bool(answer) and answer.get("id") == "srv-1" and answer.get("result") == {}
        send({"jsonrpc": "2.0", "id": rid, "result": {
            "content": [{"type": "text", "text": json.dumps({"app": "LibreCAD"})}],
            "structuredContent": {"app": "LibreCAD", "ping_ok": ok}, "isError": False}})
    elif method == "tools/call" and msg["params"]["name"] == "render_view":
        send({"jsonrpc": "2.0", "id": rid, "result": {"content": [
            {"type": "image", "data": PNG, "mimeType": "image/png"}, {"type": "text", "text": "window"}]}})
    else:
        send({"jsonrpc": "2.0", "id": rid, "error": {"code": -32601, "message": "unknown"}})
'''


def run_selftest(args: argparse.Namespace) -> int:
    r = Runner(args.verbose)
    token = secrets.token_hex(16)
    server = _stub_server(token)
    port = server.server_address[1]
    api = ApiClient(f"http://127.0.0.1:{port}", token, timeout=10)
    tmp = Path(tempfile.mkdtemp(prefix="librecad-e2e-selftest-")).resolve()
    try:
        with r.step("HTTP client"):
            resp = api.get("status")
            r.check("X-Api-Token request -> 200 JSON", resp.status == 200 and resp.obj.get("app") == "LibreCAD", resp)
            r.check("headers are lower-cased", resp.header("Cache-Control") == "no-store"
                    and "cache-control" in resp.headers, short(resp.headers))
            r.expect("Bearer -> 200", api.get("status", bearer=True), 200)
            resp = api.get("status", token=None)
            r.check("no token -> 401 unauthorized", resp.status == 401 and resp.error == "unauthorized", resp)
            r.expect("Host override -> 421", api.get("status", host="evil.example:80"), 421, "bad_host")
            r.expect("Origin header -> 403", api.get("status", headers={"Origin": "http://x"}), 403,
                     "origin_not_allowed")
            r.check("OPTIONS reaches the server", api.request("OPTIONS", "status").status == 405)
            r.expect("unknown route -> 404 not_found", api.get("nope"), 404, "not_found")
            resp = api.get("echo", {"type": ["line", "arc"], "layer": "a b/c,d", "select": True, "skip": None})
            q = resp.obj.get("query", {})
            r.check("lists repeat the key, values are percent-encoded",
                    q.get("type") == ["line", "arc"] and q.get("layer") == ["a b/c,d"]
                    and q.get("select") == ["true"] and "skip" not in q, resp)
            r.check("space is %20 and / is %2F", "a%20b%2Fc%2Cd" in resp.obj.get("raw_query", ""),
                    short(resp.obj.get("raw_query")))
            resp = api.get(f"entities/{api.segment('walls/1 a%')}")
            r.check("path parameters survive quote(x, safe='')", resp.obj.get("param") == "walls/1 a%", resp)
            body = {"name": "Mauer äöü — 墙", "n": [1, 2.5, None]}
            resp = api.post("echo", body)
            r.check("JSON body round trip (UTF-8)", resp.obj.get("body") == body and resp.obj.get("method") == "POST",
                    resp)
            r.check("an empty POST sends {}", api.post("echo").obj.get("body") == {})
            r.check("PATCH/DELETE work", api.patch("echo", {"a": 1}).obj.get("method") == "PATCH"
                    and api.delete("echo").obj.get("method") == "DELETE")

        with r.step("PNG, window and file checks"):
            resp = api.get("render", {"width": 64, "height": 48})
            r.check("png_size reads IHDR", png_size(resp.body) == (64, 48), short(png_size(resp.body)))
            r.check("parse_window", parse_window(resp.header("X-LibreCAD-Window")) == [-1.5, -2.0, 101.5, 52.0])
            r.check("parse_window rejects junk", parse_window("1,2,3") is None and parse_window("a,b,c,d") is None
                    and parse_window(None) is None and parse_window("1,2,nan,4") is None)
            r.check("png_size rejects non-PNG", png_size(b"%PDF-1.4") is None and png_size(b"") is None)
            r.check("describe() summarises binary bodies", "<png" in resp.describe())
            r.check("dwg_version", dwg_version(b"AC1032\x00\x00rest") == "AC1032" and dwg_version(b"AC10") == "AC10")
            r.check("is_pdf", is_pdf(b"%PDF-1.7\n") and not is_pdf(b"<svg"))
            r.check("is_svg", is_svg(b'<?xml version="1.0"?>\n<svg xmlns="x">') and not is_svg(b"%PDF"))
            dxf = b"  0\nSECTION\n  2\nHEADER\n" + b"x" * 1000 + b"\n  0\nEOF\n"
            r.check("looks_like_dxf", looks_like_dxf(dxf) and looks_like_dxf(b"AutoCAD Binary DXF\r\n\x1a\x00")
                    and not looks_like_dxf(b"AC1032"))
            r.check("approx", approx(5000.0000001, 5000) and not approx(5001, 5000) and not approx(True, 1)
                    and not approx(None, 0) and not approx(float("nan"), 0) and approx(1e-9, 0))
            r.check("approx_point", approx_point([150, 125.0, 0], [150, 125]) and not approx_point([150], [150, 0])
                    and not approx_point(None, [0, 0]))
            r.check("same_path", same_path(str(tmp / "a.dxf"), str(tmp) + "/./a.dxf")
                    and not same_path(str(tmp / "a.dxf"), str(tmp / "b.dxf")) and not same_path(None, "x"))

        with r.step("screening of answers"):
            api.get("dialog")
            r.check("skipped_dialogs is recorded", len(api.dialogs) == 1 and api.dialogs[0][0].endswith("/dialog"),
                    short(api.dialogs))
            api.get("leak")
            r.check("a token in an answer is recorded", len(api.leaks) == 1, short(api.leaks))
            r.check("a normal answer is not", api.get("status") and len(api.leaks) == 1 and len(api.dialogs) == 1)

        with r.step("settings, discovery and start-up polling"):
            fake_exe = tmp / "bin" / "LibreCAD.exe"
            fake_exe.parent.mkdir(parents=True)
            fake_exe.write_bytes(b"")
            r.check("a development build (no platforms folder) counts as offscreen-capable",
                    offscreen_plugin_available(fake_exe))
            (fake_exe.parent / "platforms").mkdir()
            (fake_exe.parent / "platforms" / "qwindows.dll").write_bytes(b"")
            r.check("a package without qoffscreen does not", not offscreen_plugin_available(fake_exe))
            (fake_exe.parent / "platforms" / "qoffscreen.dll").write_bytes(b"")
            r.check("a package with qoffscreen does", offscreen_plugin_available(fake_exe))

            app = LaunchedApp(fake_exe, tmp / "run", port, token, False, None)
            ini = app.write_settings()
            import configparser
            parser = configparser.ConfigParser()
            parser.optionxform = str
            parser.read(ini, encoding="utf-8")
            r.check("the INI sits at <dir>/LibreCAD/LibreCAD-2.ini",
                    ini == app.settings_dir / "LibreCAD" / "LibreCAD-2.ini")
            r.check("[Startup] turns off first-load, splash and update check",
                    parser.get("Startup", "FirstLoad") == "false" and parser.get("Startup", "ShowSplash") == "false"
                    and parser.get("Startup", "CheckForNewVersions") == "false")
            r.check("[RemoteAPI] enables the API with port and token",
                    parser.get("RemoteAPI", "Enabled") == "true" and parser.get("RemoteAPI", "Port") == str(port)
                    and parser.get("RemoteAPI", "Token") == token and len(token) == 32)
            r.check("[Paths] keeps LibreCAD's other files in the sandbox",
                    parser.get("Paths", "OtherSettingsDir", fallback="") == app.settings_dir.as_posix())
            support = REPO_ROOT / "librecad" / "support"
            if (support / "fonts").is_dir():
                r.check("a development build gets the checkout's fonts and patterns",
                        parser.get("Paths", "Fonts", fallback="") == (support / "fonts").as_posix()
                        and parser.get("Paths", "Patterns", fallback="") == (support / "patterns").as_posix())
            (fake_exe.parent / "resources" / "fonts").mkdir(parents=True)
            paths = sandbox_paths(fake_exe, app.settings_dir)
            r.check("a package with its own fonts keeps them", "Fonts" not in paths, short(paths))
            r.check("no discovery file yet -> None", read_discovery(app.discovery) is None)
            app.discovery.write_text("{not json", encoding="utf-8")
            r.check("a partial discovery file -> None", read_discovery(app.discovery) is None)
            app.discovery.write_text(json.dumps({"url": f"http://127.0.0.1:{port}", "token": token,
                                                 "pid": os.getpid()}), encoding="utf-8")
            client, info, why = app.wait_ready(5)
            r.check("wait_ready connects through the discovery file", client is not None and info["token"] == token,
                    why)
            app.discovery.write_text(json.dumps({"url": f"http://127.0.0.1:{port}", "token": "f" * 32}),
                                     encoding="utf-8")
            started = time.monotonic()
            client, info, why = app.wait_ready(20)
            r.check("a wrong token stops the wait at once with a reason",
                    client is None and "401" in why and time.monotonic() - started < 5, why)
            r.check("default_discovery_path ends in LibreCAD/LibreCAD-2/remote-api.json",
                    default_discovery_path().parts[-3:] == ("LibreCAD", "LibreCAD-2", "remote-api.json"),
                    str(default_discovery_path()))
            r.check("pid_alive: this process is alive", pid_alive(os.getpid()))
            child = subprocess.Popen([sys.executable, "-c", "pass"])
            child.wait()
            r.check("pid_alive: a finished process is not alive", not pid_alive(child.pid))
            port2 = free_port()
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", port2))
                r.check("free_port returns a bindable port", 1024 <= port2 <= 65535)

        with r.step("MCP stdio client against a stub server"):
            stub = tmp / "stub_mcp_server.py"
            stub.write_text(_STUB_MCP_SERVER, encoding="utf-8")
            png = base64.b64encode(_tiny_png(4, 3)).decode("ascii")
            client = McpStdioClient([sys.executable, "-I", str(stub), png], dict(os.environ),
                                    stderr_path=tmp / "stub.stderr.log")
            try:
                info = client.initialize(timeout=30)
                r.check("initialize skips a notification and returns the result",
                        info.get("serverInfo", {}).get("name") == "stub", short(info))
                names = [t.get("name") for t in client.list_tools()]
                r.check("tools/list follows nextCursor", names == ["get_status", "render_view"], short(names))
                result = client.call_tool("get_status", timeout=30)
                body = tool_body(result)
                r.check("tools/call answers a ping from the server first",
                        body.get("ping_ok") is True and client.server_requests == ["ping"], short(result))
                r.check("tool_body prefers structuredContent", body.get("app") == "LibreCAD")
                result = client.call_tool("render_view", timeout=30)
                images = tool_images(result)
                r.check("image content decodes to a PNG",
                        images and png_size(base64.b64decode(images[0]["data"])) == (4, 3), short(result)[:200])
                try:
                    client.call_tool("nope", timeout=30)
                    r.check("a JSON-RPC error raises McpError", False)
                except McpError:
                    r.check("a JSON-RPC error raises McpError", True)
                r.check("no stray stdout lines", not client.noise, short(client.noise))
            finally:
                client.close()
            r.check("the stub server exited when stdin closed", client.proc.returncode == 0,
                    str(client.proc.returncode))
            r.check("its stderr went to the log, not the protocol",
                    b"stderr is not protocol" in (tmp / "stub.stderr.log").read_bytes())
            r.check("tool_body falls back to the text item",
                    tool_body({"content": [{"type": "text", "text": "{\"a\": 1}"}]}) == {"a": 1}
                    and tool_body({"structuredContent": {"result": [1]}}) == [1]
                    and tool_body({"content": [{"type": "text", "text": "plain"}]}) == {"text": "plain"})

        with r.step("bookkeeping"):
            inner = Runner()
            with open(os.devnull, "w") as devnull, contextlib.redirect_stdout(devnull):
                inner.check("ok", True)
                with inner.step("broken step"):
                    raise RuntimeError("boom")
                with inner.step("required"):
                    inner.require("needed", False)
                    inner.check("never reached", True)
                code = inner.summary()
            r.check("an exception in a step becomes a FAIL and the run goes on",
                    inner.passed == 1 and len(inner.failed) == 2 and code == 1, short(inner.failed))
            r.check("ApiClient.target builds /api/v1 paths",
                    ApiClient.target("status") == "/api/v1/status"
                    and ApiClient.target("/api/v1/x", {"doc": 3}) == "/api/v1/x?doc=3")
            os.environ["LIBRECAD_E2E_SELFTEST_VALUE"] = "${user_config.api_token}"
            r.check("env_value ignores unsubstituted ${...}", env_value("LIBRECAD_E2E_SELFTEST_VALUE") is None)
            del os.environ["LIBRECAD_E2E_SELFTEST_VALUE"]
    finally:
        server.shutdown()
        server.server_close()
        shutil.rmtree(tmp, ignore_errors=True)
    return r.summary()


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="End-to-end smoke test of the LibreCAD-mcp Remote API (DESIGN.md 5.3).")
    parser.add_argument("--exe", help="LibreCAD executable to start (default: $LIBRECAD_EXE)")
    parser.add_argument("--visible", action="store_true",
                        help="show the window instead of QT_QPA_PLATFORM=offscreen")
    parser.add_argument("--keep", action="store_true", help="keep the temporary directory (settings, log, files)")
    parser.add_argument("--attach", action="store_true",
                        help="test a LibreCAD that is already running, found through its discovery file "
                             "(or LIBRECAD_API_URL and LIBRECAD_API_TOKEN); only the drawings this run "
                             "creates are touched and they are closed at the end")
    parser.add_argument("--discovery", help="--attach: path of remote-api.json (default: "
                                            "$LIBRECAD_API_DISCOVERY, else the OS default)")
    parser.add_argument("--via-mcp", action="store_true",
                        help="also drive the librecad-mcp server over stdio (needs uv, or --mcp-command)")
    parser.add_argument("--mcp-project", default=str(REPO_ROOT / "mcp"),
                        help="project directory for 'uv run --project <dir> librecad-mcp' (default: mcp/)")
    parser.add_argument("--mcp-command", help="command line that starts the MCP server instead of uv run")
    parser.add_argument("--qt-bin", help="folder prepended to PATH for LibreCAD (Qt's bin folder for a "
                                         "development build on Windows)")
    parser.add_argument("--port", type=int, default=0, help="Remote API port (default: a free one)")
    parser.add_argument("--start-timeout", type=float, default=START_TIMEOUT,
                        help="seconds to wait for the API after start (default 60)")
    parser.add_argument("--selftest", action="store_true",
                        help="check this script's HTTP, file and MCP helpers against local stubs")
    parser.add_argument("-v", "--verbose", action="store_true", help="also print details of passing checks")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    with contextlib.suppress(AttributeError, ValueError):
        sys.stdout.reconfigure(errors="replace")   # never die on a console that cannot print a detail
    if args.selftest:
        return run_selftest(args)
    return run_e2e(args)


if __name__ == "__main__":
    sys.exit(main())
