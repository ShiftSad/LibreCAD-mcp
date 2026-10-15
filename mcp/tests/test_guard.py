# SPDX-License-Identifier: GPL-2.0-or-later
"""Client-side path rules (checked per OS flavor) and the confirmation helpers."""
import os

import pytest
from mcp.server.mcpserver import AcceptedElicitation, CancelledElicitation, DeclinedElicitation
import mcp.types as types

from librecad_mcp.guard import (BlockedByPolicy, Confirmation, allow_override_warnings, can_ask_user,
                                check_path, confirmation_refusal, env_flag, env_value, resolved_path)


def reason(path, kind="open", fmt=None, **kw):
    with pytest.raises(BlockedByPolicy) as exc:
        check_path(path, kind, fmt, **kw)
    assert exc.value.body["error"] == "blocked_by_local_policy" and exc.value.body["hint"]
    return exc.value.body["reason"]


# --- Windows flavor ---------------------------------------------------------------------------

@pytest.mark.parametrize("path", [r"C:\drawings\plan.dxf", "C:/drawings/plan.DXF", r"d:\a\b\c.dwg"])
def test_windows_absolute_paths_pass(path):
    assert check_path(path, "open", windows=True, env={})


def test_windows_path_is_normalized():
    assert check_path(r"C:\a\.\b\..\plan.dxf", "open", windows=True, env={}) == r"C:\a\plan.dxf"


@pytest.mark.parametrize("path,why", [
    ("plan.dxf", "bad_path"),
    (r"drawings\plan.dxf", "bad_path"),
    ("C:plan.dxf", "bad_path"),                 # drive-relative
    (r"\plan.dxf", "bad_path"),                 # rooted without a drive
    (r"C:\a\plan.dxf:stream", "bad_path"),      # alternate data stream
    (r"C:\a\CON.dxf", "bad_path"),              # device name
    (r"C:\a\nul.dwg", "bad_path"),
    (r"C:\a\CONIN$.dxf", "bad_path"),           # console device, as LibreCAD refuses it
    ("C:\\a\\COM\u00b9.dxf", "bad_path"),         # superscript digits are reserved like plain ones
    ("C:\\a\\lpt\u00b3.dxf", "bad_path"),
    (r"C:\a\COM1 .dxf", "bad_path"),            # trailing spaces and dots do not hide a device
    (r"C:\a\nul..dxf", "bad_path"),
    ("", "bad_path"),
    ("${user_config.path}", "bad_path"),
    ("~/plan.dxf", "bad_path"),
    (r"C:\a\\", "bad_path"),
    (r"\\server\share\plan.dxf", "network_path"),
    ("//server/share/plan.dxf", "network_path"),
    (r"\\?\C:\a\plan.dxf", "device_path"),
    (r"\\.\PhysicalDrive0", "device_path"),
])
def test_windows_refusals(path, why):
    assert reason(path, windows=True, env={}) == why


def test_network_paths_need_the_explicit_opt_in():
    env = {"LIBRECAD_MCP_ALLOW_NETWORK_PATHS": "1"}
    assert check_path(r"\\server\share\plan.dxf", "open", windows=True, env=env)
    assert reason(r"\\server\share\plan.dxf", windows=True, env={"LIBRECAD_MCP_ALLOW_NETWORK_PATHS": "${x}"}) == "network_path"


@pytest.mark.parametrize("path", [r"\\?\C:\a\host.txt:x.pdf", r"\\.\C:\a\host.txt:x.pdf", "//?/C:/a/host.txt:x.pdf",
                                  r"\??\C:\a\plan.pdf", r"\\?\C:\a\plan.pdf", r"\\?\UNC\server\share\plan.pdf"])
def test_device_namespace_paths_are_refused_even_when_network_paths_are_allowed(path):
    # a local file through \\?\ would skip the drive and stream rules (LibreCAD refuses these too)
    assert reason(path, "export", "pdf", windows=True, env={"LIBRECAD_MCP_ALLOW_NETWORK_PATHS": "1"}) == "device_path"


def test_a_stream_on_a_share_is_refused():
    env = {"LIBRECAD_MCP_ALLOW_NETWORK_PATHS": "1"}
    assert reason(r"\\server\share\host.txt:x.pdf", "export", "pdf", windows=True, env=env) == "bad_path"
    assert check_path(r"\\server\share\plan.pdf", "export", "pdf", windows=True, env=env)


def test_windows_allowed_roots_are_case_insensitive_and_not_prefix_tricks():
    roots = [r"C:\Users\Me\CAD"]
    assert check_path(r"c:\users\me\cad\a\plan.dxf", "open", windows=True, env={}, roots=roots)
    assert reason(r"C:\Users\Me\CADX\plan.dxf", windows=True, env={}, roots=roots) == "outside_allowed_roots"
    assert reason(r"C:\Users\Me\CAD\..\secret\plan.dxf", windows=True, env={}, roots=roots) == "outside_allowed_roots"


# --- POSIX flavor ------------------------------------------------------------------------------

def test_posix_rules():
    assert check_path("/home/me/plan.dxf", "open", windows=False, env={}) == "/home/me/plan.dxf"
    assert check_path("/home/me/../me/plan.dxf", "open", windows=False, env={}) == "/home/me/plan.dxf"
    assert reason("plan.dxf", windows=False, env={}) == "bad_path"
    assert reason("//host/share/plan.dxf", windows=False, env={}) == "network_path"
    assert reason(r"C:\plan.dxf", windows=False, env={}) == "bad_path"


def test_posix_allowed_roots():
    roots = ["/srv/cad"]
    assert check_path("/srv/cad/x/plan.dxf", "open", windows=False, env={}, roots=roots)
    assert reason("/srv/cadx/plan.dxf", windows=False, env={}, roots=roots) == "outside_allowed_roots"
    assert reason("/srv/cad/../etc/plan.dxf", windows=False, env={}, roots=roots) == "outside_allowed_roots"


def test_native_allowed_roots_from_the_environment(tmp_path):
    inside = tmp_path / "cad"
    inside.mkdir()
    env = {"LIBRECAD_MCP_ALLOWED_ROOTS": os.pathsep.join([str(inside), str(tmp_path / "other")])}
    assert check_path(str(inside / "plan.dxf"), "open", env=env)
    with pytest.raises(BlockedByPolicy) as exc:
        check_path(str(tmp_path / "plan.dxf"), "open", env=env)
    assert exc.value.body["reason"] == "outside_allowed_roots" and str(inside) in exc.value.body["hint"]


# --- extensions per format -------------------------------------------------------------------------

@pytest.mark.parametrize("name", ["a.dxf", "a.DWG", "a.lff", "a.cxf", "a.jww", "a.jwc", "a.shp"])
def test_open_accepts_librecad_file_types(name):
    assert check_path("/x/" + name, "open", windows=False, env={})


@pytest.mark.parametrize("name", ["a.txt", "a", "a.dxf.exe", "a.pdf"])
def test_open_refuses_other_types(name):
    assert reason("/x/" + name, "open", windows=False, env={}) == "extension_mismatch"


@pytest.mark.parametrize("kind,fmt,name,ok", [
    ("save", "dxf", "a.dxf", True), ("save", "dwg", "a.DWG", True), ("save", "dwg", "a.dxf", False),
    ("export", "pdf", "a.pdf", True), ("export", "png", "a.png", True), ("export", "svg", "a.svg", True),
    ("export", "png", "a.jpg", False), ("export", "dxf", "a.dwg", False),
])
def test_written_files_need_the_format_extension(kind, fmt, name, ok):
    if ok:
        assert check_path("/x/" + name, kind, fmt, windows=False, env={})
    else:
        assert reason("/x/" + name, kind, fmt, windows=False, env={}) == "extension_mismatch"


def test_unknown_format():
    assert reason("/x/a.jpg", "export", "jpg", windows=False, env={}) == "unsupported_format"


def _alias_of(target):
    """A second name of target ending in .dxf: its 8.3 alias on Windows, a symbolic link elsewhere;
    None when the system has neither."""
    if os.name == "nt":
        import ctypes
        buffer = ctypes.create_unicode_buffer(4096)
        n = ctypes.windll.kernel32.GetShortPathNameW(str(target), buffer, len(buffer))
        alias = buffer.value if 0 < n < len(buffer) else ""
        return alias if alias and os.path.basename(alias).lower() != target.name.lower() else None
    link = target.with_name("link.dxf")
    try:
        link.symlink_to(target)
    except OSError:
        return None
    return str(link)


def test_an_existing_file_is_judged_by_its_own_name(tmp_path):
    # KEEPME~1.DXF passes as a .dxf, but replacing it destroys keepme.dxfold (LibreCAD refuses it too)
    kept = tmp_path / "keepme.dxfold"
    kept.write_bytes(b"keep")
    alias = _alias_of(kept)
    if alias is None:
        pytest.skip("no 8.3 names or symbolic links here")
    assert alias.lower().endswith(".dxf")
    with pytest.raises(BlockedByPolicy) as exc:
        check_path(alias, "export", "dxf")
    assert exc.value.body["reason"] == "extension_mismatch"
    assert "keepme.dxfold" in exc.value.body["detail"] and "keepme.dxfold" in exc.value.body["hint"]
    assert os.path.normcase(resolved_path(alias)) == os.path.normcase(os.path.realpath(kept))
    # its own name, and a new file, need no second look
    assert resolved_path(str(kept)) is None
    assert resolved_path(str(tmp_path / "new.dxf")) is None
    good = tmp_path / "a long drawing name.dxf"
    good.write_bytes(b"0\nEOF\n")
    if os.name == "nt" and (good_alias := _alias_of(good)) is not None:
        assert check_path(good_alias, "export", "dxf")


# --- environment hygiene ---------------------------------------------------------------------------

def test_env_value_ignores_blank_and_placeholders():
    env = {"A": "  x ", "B": " ", "C": "${user_config.c}"}
    assert env_value("A", env) == "x"
    assert env_value("B", env) is None and env_value("C", env) is None and env_value("D", env) is None


@pytest.mark.parametrize("value,expected", [("1", True), ("true", True), ("YES", True), ("on", True),
                                            ("0", False), ("no", False), ("maybe", False), ("${x}", False)])
def test_env_flag(value, expected):
    assert env_flag("F", {"F": value}) is expected


def test_startup_warnings():
    env = {"LIBRECAD_MCP_ALLOWED_ROOTS": "relative/dir", "LIBRECAD_MCP_ALLOW_NETWORK_PATHS": "maybe",
           "LIBRECAD_API_URL": "http://192.0.2.5:13140"}
    warnings = allow_override_warnings(env)
    assert len(warnings) == 3
    assert all(w.startswith("librecad-mcp: ") for w in warnings)
    assert allow_override_warnings({}) == []


# --- confirmations ---------------------------------------------------------------------------------

def test_can_ask_user_reads_the_client_capabilities():
    class Ctx:
        def __init__(self, elicitation):
            self.client_capabilities = types.ClientCapabilities(elicitation=elicitation)
    assert can_ask_user(Ctx(types.ElicitationCapability()))
    assert can_ask_user(Ctx(types.ElicitationCapability(form=types.FormElicitationCapability())))
    assert not can_ask_user(Ctx(types.ElicitationCapability(url=types.UrlElicitationCapability())))
    assert not can_ask_user(Ctx(None))
    assert not can_ask_user(None)


def test_a_seen_prompt_beats_user_confirmed():
    yes = AcceptedElicitation(data=Confirmation(confirm=True))
    assert confirmation_refusal(yes, False, "q?", True) is None
    unticked = AcceptedElicitation(data=Confirmation(confirm=False))
    for answer, how in [(DeclinedElicitation(), "said no"), (CancelledElicitation(), "closed"),
                        (unticked, "did not tick")]:
        out = confirmation_refusal(answer, True, "q?", True)
        assert out["error"] == "not_confirmed" and how in out["message"]


def test_without_a_prompt():
    # what the SDK injects when the resolver asked nothing
    no_question = AcceptedElicitation.model_construct(action="accept", data=None)
    # a client that can ask but was not asked: user_confirmed never stands in for the prompt
    assert confirmation_refusal(no_question, True, "q?", True)["error"] == "not_confirmed"
    # a client that cannot ask: user_confirmed decides
    assert confirmation_refusal(None, True, "q?", False) is None
    out = confirmation_refusal(None, False, "Delete it?", False)
    assert out == {"error": "confirmation_required", "message": "Delete it?", "hint": out["hint"]}
    assert "user_confirmed=True" in out["hint"]


@pytest.mark.parametrize("windows", [True, False])
@pytest.mark.parametrize("bad", [chr(0), chr(1), chr(10), chr(0x1F), chr(0x7F)])
def test_control_characters_are_refused(windows, bad):
    # "run.bat<NUL>.dxf" would pass the .dxf check while the system opens run.bat
    path = ("C:/a/run.bat" if windows else "/a/run.bat") + bad + ".dxf"
    with pytest.raises(BlockedByPolicy) as exc:
        check_path(path, "export", "dxf", windows=windows, env={})
    assert exc.value.body["reason"] == "bad_path"
    assert bad not in exc.value.body["path"]
