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
    return [base / _ORGANIZATION / _APPLICATION / DISCOVERY_FILE_NAME]


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
