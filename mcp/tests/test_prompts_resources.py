# SPDX-License-Identifier: GPL-2.0-or-later
import json

from mcp.client.client import Client

import librecad_mcp.server as srv


async def test_guide_resource_is_offline():
    async with Client(srv.mcp, mode="legacy") as client:
        resources = {str(r.uri) for r in (await client.list_resources()).resources}
        assert resources == {"librecad://guide", "librecad://routes"}
        result = await client.read_resource("librecad://guide")
    text = result.contents[0].text
    assert text.startswith("# LibreCAD MCP guide") and "Degrees" in text and "not undoable" in text


async def test_routes_resource_proxies_the_live_route_list(fake):
    data = json.loads(await srv.routes_resource())
    assert data["api_version"] == "1.0"
    assert {"method": "GET", "path": "/api/v1/status"}.items() <= data["routes"][0].items()


async def test_routes_resource_without_librecad_is_an_error_text():
    data = json.loads(await srv.routes_resource())
    assert "not reachable" in data["error"]


async def test_prompts():
    async with Client(srv.mcp, mode="legacy") as client:
        names = {p.name for p in (await client.list_prompts()).prompts}
        assert names == {"draw-from-description", "inspect-drawing", "convert-file"}
        draw = await client.get_prompt("draw-from-description", {"description": "a 10 m x 5 m room"})
        convert = await client.get_prompt("convert-file", {"path": "C:/a.dwg", "format": "dxf", "version": "R12"})
        inspect_ = await client.get_prompt("inspect-drawing", {})
    draw_text = draw.messages[0].content.text
    assert "a 10 m x 5 m room" in draw_text and "add_entities" in draw_text and "render_view" in draw_text
    convert_text = convert.messages[0].content.text
    assert all(t in convert_text for t in ("open_document", "export_document", "close_document", "R12"))
    assert "get_drawing_info" in inspect_.messages[0].content.text


def test_server_instructions_and_metadata():
    assert "get_status first" in srv.mcp.instructions
    assert srv.mcp.name == "librecad"
