# SPDX-License-Identifier: GPL-2.0-or-later
"""The release metadata agrees with itself and with the server: versions, the pinned git tag,
the tool and prompt lists of the Claude Desktop extension, and the environment it passes."""
import json
import re
import struct
import tomllib
from pathlib import Path

import anyio

import librecad_mcp.server as srv

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = json.loads((ROOT / "mcpb" / "manifest.json").read_text(encoding="utf-8"))
SERVER_JSON = json.loads((ROOT / "server.json").read_text(encoding="utf-8"))
PYPROJECT = tomllib.loads((ROOT / "pyproject.toml").read_text(encoding="utf-8"))
PIN = re.compile(r"LibreCAD-mcp@([^#\"\s]+)#subdirectory=mcp")


def test_versions_agree():
    version = PYPROJECT["project"]["version"]
    assert MANIFEST["version"] == SERVER_JSON["version"] == version


def test_the_source_is_pinned_to_one_release_tag_everywhere():
    tags = set()
    for f in (ROOT / "mcpb" / "server" / "index.js", ROOT / "server.json", ROOT / "README.md"):
        found = PIN.findall(f.read_text(encoding="utf-8"))
        assert found, f
        tags.update(found)
    assert len(tags) == 1
    assert re.fullmatch(r"v[0-9]+(\.[0-9]+)+-mcp\.[0-9]+", tags.pop())


def test_manifest_lists_every_tool_and_prompt():
    tools = {t.name for t in anyio.run(srv.mcp.list_tools)}
    assert {t["name"] for t in MANIFEST["tools"]} == tools
    prompts = {p.name for p in anyio.run(srv.mcp.list_prompts)}
    assert {p["name"] for p in MANIFEST["prompts"]} == prompts


def test_manifest_passes_only_variables_the_server_reads():
    env = MANIFEST["server"]["mcp_config"]["env"]
    assert set(env) == {"LIBRECAD_API_TOKEN", "LIBRECAD_API_URL", "LIBRECAD_MCP_ALLOWED_ROOTS"}
    for value in env.values():
        key = re.fullmatch(r"\$\{user_config\.(\w+)\}", value).group(1)
        assert MANIFEST["user_config"][key]["required"] is False  # zero configuration on one machine
    assert MANIFEST["user_config"]["api_token"]["sensitive"] is True
    registry = {v["name"] for v in SERVER_JSON["_meta"]["io.modelcontextprotocol.registry/publisher-provided"]
                ["environmentVariables"]}
    assert set(env) <= registry


def test_package_metadata():
    project = PYPROJECT["project"]
    assert project["name"] == "librecad-mcp" and project["license"]["text"] == "GPL-2.0-or-later"
    assert project["scripts"] == {"librecad-mcp": "librecad_mcp.server:main"}
    assert any(d.startswith("mcp>=2.2,<3") for d in project["dependencies"])
    assert MANIFEST["license"] == "GPL-2.0-or-later" and MANIFEST["manifest_version"] == "0.3"


def test_icon_is_a_512_png():
    data = (ROOT / "mcpb" / MANIFEST["icon"]).read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n" and struct.unpack(">II", data[16:24]) == (512, 512)


def test_version_and_help_flags(capsys):
    srv.main(["--version"])
    assert capsys.readouterr().out.startswith("librecad-mcp ")
    srv.main(["--help"])
    assert "Remote API (MCP)" in capsys.readouterr().out
