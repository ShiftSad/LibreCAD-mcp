# SPDX-License-Identifier: GPL-2.0-or-later
"""Where to reach LibreCAD's Remote API, and with which token.

Lookup order (DESIGN.md section 1.4): the environment (``LIBRECAD_API_URL``,
``LIBRECAD_API_TOKEN``), then the discovery file named by ``LIBRECAD_API_DISCOVERY``, then the
discovery file at the OS default location. Nothing is needed when LibreCAD runs on this machine
with its Remote API enabled. The config is loaded on every tool call, so ``tools/list`` works
without LibreCAD and a token change in LibreCAD is picked up without restarting this server.
"""
from __future__ import annotations

import os
from collections.abc import Mapping
from dataclasses import dataclass, field
from pathlib import Path

from .discovery import default_paths, is_stale, read_discovery
from .errors import ConfigError
from .guard import env_value

DEFAULT_URL = "http://127.0.0.1:13140"
DEFAULT_TIMEOUT = 30.0
# Open, save, export and new can take long; Claude Desktop gives up on a tool call after about
# 60 s, so the long calls stop waiting at 50 s and answer timed_out instead.
LONG_TIMEOUT = 50.0

ENABLE_HINT = ("Start LibreCAD (the LibreCAD-mcp build) and tick Options > Application Preferences > "
               "Remote API (MCP) > Enable Remote API, or set LIBRECAD_API_URL and LIBRECAD_API_TOKEN.")


@dataclass(frozen=True)
class Config:
    base_url: str
    token: str = field(repr=False)
    timeout: float = DEFAULT_TIMEOUT
    token_source: str = ""             # "LIBRECAD_API_TOKEN" or "discovery file"
    discovery_path: str | None = None  # the discovery file used, if any

    def describe(self) -> dict:
        """Connection facts safe to show the model: never the token."""
        return {"url": self.base_url, "token_source": self.token_source,
                "discovery_file": self.discovery_path, "timeout_s": self.timeout}


def _clean_url(url: str, origin: str) -> str:
    url = url.strip().rstrip("/")
    if url.endswith("/api/v1"):
        url = url[: -len("/api/v1")]
    if not url.startswith(("http://", "https://")) or len(url.split("://", 1)[1]) == 0:
        raise ConfigError(f"{origin} must look like http://127.0.0.1:13140, got {url!r}")
    return url


def _check_token(token: str, origin: str) -> str:
    token = token.strip()
    if not token.isascii() or not token.isprintable() or " " in token:
        # Never echo the value: it is a secret even when it is malformed.
        raise ConfigError(f"{origin} holds characters that cannot be sent in an HTTP header; "
                          "copy the token again from Preferences > Remote API (MCP)")
    return token


def load_config(env: Mapping[str, str] | None = None) -> Config:
    env = os.environ if env is None else env

    timeout_text = env_value("LIBRECAD_API_TIMEOUT", env)
    timeout = DEFAULT_TIMEOUT
    if timeout_text is not None:
        try:
            timeout = float(timeout_text)
        except ValueError:
            raise ConfigError(f"LIBRECAD_API_TIMEOUT must be a number of seconds, got {timeout_text!r}") from None
        if not (0 < timeout <= 600):
            raise ConfigError(f"LIBRECAD_API_TIMEOUT must be between 0 and 600 seconds, got {timeout_text!r}")

    url = env_value("LIBRECAD_API_URL", env)
    token = env_value("LIBRECAD_API_TOKEN", env)
    if url is not None:
        url = _clean_url(url, "LIBRECAD_API_URL")
    if token is not None:
        token = _check_token(token, "LIBRECAD_API_TOKEN")
        if url is None:
            # The token came from the environment; the discovery file may still know the port.
            disc_path = _explicit_discovery(env) or _first_existing(default_paths(env=env))
            try:
                disc = read_discovery(disc_path) if disc_path else None
            except ValueError:
                disc = None
            if disc and not is_stale(disc):
                return Config(disc["url"], token, timeout, "LIBRECAD_API_TOKEN", str(disc_path))
            url = DEFAULT_URL
        return Config(url, token, timeout, "LIBRECAD_API_TOKEN", None)

    explicit = _explicit_discovery(env)
    candidates = [explicit] if explicit else default_paths(env=env)
    stale: list[str] = []
    for path in candidates:
        try:
            disc = read_discovery(path)
        except ValueError as e:
            raise ConfigError(f"{e}. {ENABLE_HINT}") from e
        if disc is None:
            continue
        if is_stale(disc):
            # Left behind by a LibreCAD that was killed or crashed: never send its token anywhere.
            stale.append(f"{path} (process {disc.get('pid')} is not running)")
            continue
        disc_token = _check_token(disc["token"], f"discovery file {path}")
        return Config(url or disc["url"], disc_token, timeout, "discovery file", str(path))

    origin = " (LIBRECAD_API_DISCOVERY)" if explicit else ""
    if stale:
        raise ConfigError(f"LibreCAD not reachable: the Remote API discovery file {'; '.join(stale)}{origin} "
                          f"was left by a LibreCAD that is no longer running. {ENABLE_HINT}")
    where = " or ".join(str(p) for p in candidates)
    raise ConfigError(f"LibreCAD not reachable: no Remote API discovery file at {where}{origin}, "
                      f"so LibreCAD is not running with its Remote API enabled. {ENABLE_HINT}")


def _explicit_discovery(env: Mapping[str, str]) -> Path | None:
    value = env_value("LIBRECAD_API_DISCOVERY", env)
    return Path(os.path.expanduser(value)) if value else None


def _first_existing(paths: list[Path]) -> Path | None:
    for p in paths:
        if p.is_file():
            return p
    return None
