# SPDX-License-Identifier: GPL-2.0-or-later
"""LibreCadClient against respx-mocked HTTP: headers, parameter encoding and error mapping."""
import httpx
import pytest
import respx

from librecad_mcp.client import LibreCadClient
from librecad_mcp.config import Config
from librecad_mcp.errors import Conflict, NotFound, NotReachable, UiTimeout, Unauthorized, Validation
from fake_librecad import PNG_1X1

BASE = "http://127.0.0.1:13140"
API = BASE + "/api/v1"


def client(token="tok"):
    return LibreCadClient(Config(BASE, token, 5.0, "LIBRECAD_API_TOKEN"))


@respx.mock
async def test_token_header_and_json():
    route = respx.get(f"{API}/status").mock(return_value=httpx.Response(200, json={"app": "LibreCAD"}))
    async with client("s3cret") as c:
        assert (await c.status()) == {"app": "LibreCAD"}
    req = route.calls.last.request
    assert req.headers["x-api-token"] == "s3cret"
    assert req.headers["user-agent"].startswith("librecad-mcp/")
    assert "origin" not in req.headers
    assert req.headers["host"] == "127.0.0.1:13140"


@respx.mock
async def test_document_id_goes_into_the_doc_query_parameter():
    route = respx.get(f"{API}/drawing").mock(return_value=httpx.Response(200, json={}))
    async with client() as c:
        await c.drawing(3)
        await c.drawing(None)
    assert route.calls[0].request.url.params["doc"] == "3"
    assert "doc" not in route.calls[1].request.url.params


@respx.mock
async def test_repeated_type_and_layer_parameters():
    route = respx.get(f"{API}/entities").mock(return_value=httpx.Response(200, json={"entities": []}))
    async with client() as c:
        await c.entities(None, {"type": ["line", "arc"], "layer": ["walls", "a b,c"], "bbox": "0,0,10,10",
                                "limit": 5, "selected": None})
    params = route.calls.last.request.url.params
    assert params.get_list("type") == ["line", "arc"]
    assert params.get_list("layer") == ["walls", "a b,c"]  # layer names are never comma-split
    assert params["bbox"] == "0,0,10,10" and params["limit"] == "5"
    assert "selected" not in params  # None values are not sent


@respx.mock
async def test_layer_names_are_percent_encoded_in_the_path():
    route = respx.patch(url__regex=r".*/api/v1/layers/.*").mock(return_value=httpx.Response(200, json={}))
    async with client() as c:
        await c.update_layer(None, "Walls/Ext 1%", {"color": "#ff0000"})
    raw = route.calls.last.request.url.raw_path.decode()
    assert raw == "/api/v1/layers/Walls%2FExt%201%25"


@respx.mock
async def test_delete_layer_sends_policy_in_the_query():
    route = respx.delete(f"{API}/layers/old").mock(return_value=httpx.Response(200, json={}))
    async with client() as c:
        await c.delete_layer(1, "old", "move", "0")
        await c.delete_layer(1, "old", "refuse", "0")
    assert dict(route.calls[0].request.url.params) == {"entities": "move", "move_to": "0", "doc": "1"}
    assert "move_to" not in route.calls[1].request.url.params


@respx.mock
async def test_booleans_are_lowercase_query_values():
    route = respx.get(f"{API}/render").mock(return_value=httpx.Response(
        200, content=PNG_1X1, headers={"content-type": "image/png", "x-librecad-window": "0,0,10,5",
                                       "x-librecad-scale": "100"}))
    async with client() as c:
        png, meta = await c.render(None, mode="extents", monochrome=True, width=64)
    assert route.calls.last.request.url.params["monochrome"] == "true"
    assert png == PNG_1X1
    assert meta == {"window": {"min": [0.0, 0.0], "max": [10.0, 5.0]}, "px_per_unit": 100.0, "empty": False}


@respx.mock
async def test_render_of_an_empty_drawing_and_bad_headers():
    respx.get(f"{API}/render").mock(return_value=httpx.Response(
        200, content=PNG_1X1, headers={"content-type": "image/png", "x-librecad-empty": "1",
                                       "x-librecad-window": "garbage"}))
    async with client() as c:
        _, meta = await c.render(None)
    assert meta == {"window": None, "px_per_unit": None, "empty": True}


@respx.mock
async def test_timeout_becomes_a_client_side_ui_timeout():
    respx.post(f"{API}/documents/save").mock(side_effect=httpx.ReadTimeout("slow"))
    async with client() as c:
        with pytest.raises(UiTimeout) as exc:
            await c.save_document()
    assert exc.value.client_side


@respx.mock
async def test_connection_refused_is_not_reachable():
    respx.get(f"{API}/status").mock(side_effect=httpx.ConnectError("refused"))
    async with client() as c:
        with pytest.raises(NotReachable) as exc:
            await c.status()
    assert "not reachable at http://127.0.0.1:13140" in str(exc.value)
    assert "Remote API (MCP)" in str(exc.value)


@respx.mock
async def test_error_bodies_are_mapped_and_kept():
    respx.post(f"{API}/documents/save_as").mock(return_value=httpx.Response(
        409, json={"error": "file_exists", "path": "C:/a.dxf"}))
    respx.post(f"{API}/entities").mock(return_value=httpx.Response(
        422, json={"error": "invalid_entity", "errors": {"entities[0].radius": "must be > 0"}}))
    respx.get(f"{API}/nope").mock(return_value=httpx.Response(404, json={"error": "not_found"}))
    async with client() as c:
        with pytest.raises(Conflict) as exc:
            await c.save_document_as(None, "C:/a.dxf")
        assert exc.value.body["path"] == "C:/a.dxf"
        with pytest.raises(Validation) as exc2:
            await c.create_entities(None, {"entities": [{"type": "circle"}]})
        assert exc2.value.errors == {"entities[0].radius": "must be > 0"}
        with pytest.raises(NotFound) as exc3:
            await c._request("GET", "/nope")
        assert exc3.value.route_missing


@respx.mock
async def test_unauthorized_from_something_that_is_not_librecad_gets_a_port_hint():
    respx.get(f"{API}/status").mock(return_value=httpx.Response(401, json={"error": "unauthorized"}))
    async with client() as c:
        with pytest.raises(Unauthorized) as exc:
            await c.status()
    assert "did not identify itself as LibreCAD" in exc.value.body["hint"]


@respx.mock
async def test_unauthorized_from_librecad_has_no_port_hint():
    respx.get(f"{API}/status").mock(return_value=httpx.Response(401, json={"error": "unauthorized", "app": "LibreCAD"}))
    async with client() as c:
        with pytest.raises(Unauthorized) as exc:
            await c.status()
    assert "hint" not in exc.value.body


@respx.mock
async def test_long_operations_use_the_50_second_cap():
    route = respx.post(f"{API}/documents/open").mock(return_value=httpx.Response(201, json={}))
    async with client() as c:
        await c.open_document("C:/a.dxf")
    assert route.calls.last.request.extensions["timeout"]["read"] == 50.0


@respx.mock
async def test_the_client_tells_librecad_its_deadline():
    # LibreCAD answers 504 (with "started") before this call gives up, and never runs a request
    # whose caller is gone
    status = respx.get(f"{API}/status").mock(return_value=httpx.Response(200, json={}))
    open_ = respx.post(f"{API}/documents/open").mock(return_value=httpx.Response(201, json={}))
    async with client() as c:  # 5 s
        await c.status()
        await c.open_document("C:/a.dxf")  # 50 s
    assert status.calls.last.request.headers["x-request-timeout-ms"] == "4500"
    assert open_.calls.last.request.headers["x-request-timeout-ms"] == "48000"


def test_proxies_from_the_environment_are_ignored(monkeypatch):
    monkeypatch.setenv("HTTP_PROXY", "http://proxy.invalid:3128")
    c = client()
    assert c._http._trust_env is False
