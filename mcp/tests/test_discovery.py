# SPDX-License-Identifier: GPL-2.0-or-later
import json
from pathlib import Path, PureWindowsPath

import pytest

from librecad_mcp.discovery import default_paths, read_discovery


def test_windows_default_is_appdata_librecad_2():
    paths = default_paths("win32", {"APPDATA": r"C:\Users\me\AppData\Roaming"}, Path("C:/Users/me"))
    assert PureWindowsPath(paths[0]) == PureWindowsPath(r"C:\Users\me\AppData\Roaming\LibreCAD\LibreCAD-2\remote-api.json")


def test_windows_without_appdata_falls_back_to_roaming_in_home():
    paths = default_paths("win32", {}, Path("/home/me"))
    assert paths[0].as_posix().endswith("AppData/Roaming/LibreCAD/LibreCAD-2/remote-api.json")


def test_linux_default_honours_xdg_data_home():
    assert default_paths("linux", {"XDG_DATA_HOME": "/data/me"}, Path("/home/me"))[0].as_posix() == \
        "/data/me/LibreCAD/LibreCAD-2/remote-api.json"
    assert default_paths("linux", {}, Path("/home/me"))[0].as_posix() == \
        "/home/me/.local/share/LibreCAD/LibreCAD-2/remote-api.json"


def test_linux_ignores_a_relative_xdg_data_home():
    # A relative XDG_DATA_HOME is invalid per the XDG spec; on POSIX Qt ignores it too.
    if Path("rel").is_absolute():  # pragma: no cover - never
        pytest.skip()
    out = default_paths("linux", {"XDG_DATA_HOME": "rel"}, Path("/home/me"))[0].as_posix()
    assert out.endswith(".local/share/LibreCAD/LibreCAD-2/remote-api.json")


def test_macos_default_is_application_support():
    assert default_paths("darwin", {}, Path("/Users/me"))[0].as_posix() == \
        "/Users/me/Library/Application Support/LibreCAD/LibreCAD-2/remote-api.json"


def write(tmp_path, data, name="remote-api.json", raw=None):
    p = tmp_path / name
    p.write_text(raw if raw is not None else json.dumps(data), encoding="utf-8")
    return p


def test_missing_file_is_none(tmp_path):
    assert read_discovery(tmp_path / "nope.json") is None


def test_valid_file(tmp_path):
    p = write(tmp_path, {"url": "http://127.0.0.1:13150/", "port": 13150, "token": "abc", "pid": 7,
                         "app": "LibreCAD", "api_version": "1.0"})
    d = read_discovery(p)
    assert d["url"] == "http://127.0.0.1:13150" and d["token"] == "abc" and d["pid"] == 7


def test_port_only_builds_a_loopback_url(tmp_path):
    assert read_discovery(write(tmp_path, {"port": 13141, "token": "t"}))["url"] == "http://127.0.0.1:13141"


def test_utf8_bom_is_accepted(tmp_path):
    p = tmp_path / "remote-api.json"
    p.write_bytes(b"\xef\xbb\xbf" + json.dumps({"port": 1, "token": "t"}).encode())
    assert read_discovery(p)["token"] == "t"


@pytest.mark.parametrize("data,needle", [
    ({"url": "http://127.0.0.1:1"}, "no token"),
    ({"url": "http://127.0.0.1:1", "token": "${user_config.api_token}"}, "no token"),
    ({"token": "secret-value"}, "neither a url nor a port"),
    ({"url": "ftp://x", "token": "secret-value"}, "unusable url"),
    ([1, 2], "JSON object"),
])
def test_unusable_files_raise_without_echoing_the_token(tmp_path, data, needle):
    p = write(tmp_path, data)
    with pytest.raises(ValueError) as exc:
        read_discovery(p)
    assert needle in str(exc.value) and str(p) in str(exc.value)
    assert "secret-value" not in str(exc.value)


def test_invalid_json(tmp_path):
    with pytest.raises(ValueError, match="not valid JSON"):
        read_discovery(write(tmp_path, None, raw="{oops"))
