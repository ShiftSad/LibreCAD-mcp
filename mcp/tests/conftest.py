# SPDX-License-Identifier: GPL-2.0-or-later
import os

import pytest

import librecad_mcp.server as srv
from fake_librecad import FakeLibreCAD
from librecad_mcp.client import LibreCadClient
from librecad_mcp.config import Config

TOKEN = "f00dfeed0123456789abcdefcafe0000"


@pytest.fixture(autouse=True)
def _isolate_env(monkeypatch, tmp_path_factory):
    """No test may read the real discovery file or inherit the developer's LIBRECAD_* settings:
    the home and per-user data folders point at an empty temp folder (POSIX reads HOME, Windows
    USERPROFILE and APPDATA), and every LIBRECAD_* variable is cleared."""
    home = tmp_path_factory.mktemp("home")
    monkeypatch.setenv("HOME", str(home))
    monkeypatch.setenv("USERPROFILE", str(home))
    monkeypatch.setenv("APPDATA", str(home / "AppData" / "Roaming"))
    monkeypatch.setenv("LOCALAPPDATA", str(home / "AppData" / "Local"))
    monkeypatch.setenv("XDG_DATA_HOME", str(home / ".local" / "share"))
    for name in list(os.environ):
        if name.startswith("LIBRECAD_"):
            monkeypatch.delenv(name, raising=False)


@pytest.fixture
def fake(monkeypatch):
    """A stateful fake LibreCAD; every tool's client talks to it."""
    f = FakeLibreCAD(token=TOKEN)
    cfg = Config("http://127.0.0.1:13140", TOKEN, 30.0, "discovery file", "C:/fake/remote-api.json")
    monkeypatch.setattr(srv, "_client", lambda: LibreCadClient(cfg, transport=f.transport()))
    return f


@pytest.fixture
def doc(fake):
    """The fake with one empty, active drawing."""
    return fake.add_document()
