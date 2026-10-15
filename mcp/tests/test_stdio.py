# SPDX-License-Identifier: GPL-2.0-or-later
"""The real server process over stdio: it starts, lists every tool, and answers get_status with a
clean error when LibreCAD is not running. Nothing but protocol may reach stdout."""
import json
import os
import sys

from mcp.client.client import Client
from mcp.client.stdio import StdioServerParameters

from test_tool_annotations import EXPECTED_TOOLS


async def test_stdio_server_lists_tools_and_answers_without_librecad(tmp_path):
    env = {k: v for k, v in os.environ.items() if not k.startswith("LIBRECAD_")}
    env["LIBRECAD_API_DISCOVERY"] = str(tmp_path / "remote-api.json")  # does not exist
    params = StdioServerParameters(command=sys.executable, args=["-m", "librecad_mcp"], env=env,
                                   cwd=str(tmp_path))
    async with Client(params, mode="legacy") as client:
        tools = {t.name for t in (await client.list_tools()).tools}
        result = await client.call_tool("get_status", {})
    assert tools == EXPECTED_TOOLS
    assert not result.is_error
    out = json.loads(result.content[0].text)
    assert "not reachable" in out["error"] and "remote-api.json" in out["error"]
