# SPDX-License-Identifier: GPL-2.0-or-later
"""The tools against the stateful fake LibreCAD (tests/fake_librecad.py)."""
import json
import socket

import pytest
from mcp.server.mcpserver import Image

import librecad_mcp.server as srv
from conftest import TOKEN
from fake_librecad import PNG_1X1
from helpers import call, call_raw, requests
from librecad_mcp.client import LibreCadClient
from librecad_mcp.config import Config

SQUARE = {"type": "polyline", "vertices": [[0, 0], [10, 0], [10, 10], [0, 10]], "closed": True}


# --- status and reachability ---------------------------------------------------------------------

async def test_get_status_reports_the_connection_but_never_the_token(fake, doc):
    out = await srv.get_status()
    assert out["app"] == "LibreCAD" and out["active_document"] == doc.id
    assert out["connection"]["url"] == "http://127.0.0.1:13140"
    assert out["connection"]["token_source"] == "discovery file"
    assert "mcp_server_version" in out["connection"]
    assert TOKEN not in json.dumps(out)
    assert fake.calls[-1]["headers"]["x-api-token"] == TOKEN


async def test_not_reachable_without_librecad():
    out = await call("get_status")
    assert "not reachable" in out["error"] and "Remote API (MCP)" in out["error"]


async def test_not_reachable_with_a_stale_discovery_file(tmp_path, monkeypatch):
    with socket.socket() as s:  # a port nothing listens on
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    p = tmp_path / "remote-api.json"
    p.write_text(json.dumps({"url": f"http://127.0.0.1:{port}", "token": "t", "pid": 1}), encoding="utf-8")
    monkeypatch.setenv("LIBRECAD_API_DISCOVERY", str(p))
    out = await srv.get_status()
    assert out["error"].startswith(f"LibreCAD not reachable at http://127.0.0.1:{port}")
    assert out["connection"]["discovery_file"] == str(p)


async def test_wrong_token(fake, monkeypatch):
    cfg = Config("http://127.0.0.1:13140", "stale", 30.0, "LIBRECAD_API_TOKEN")
    monkeypatch.setattr(srv, "_client", lambda: LibreCadClient(cfg, transport=fake.transport()))
    out = await srv.list_documents()
    assert out["error"].startswith("unauthorized (check the token") and out["app"] == "LibreCAD"


# A minimal valid call of every tool, for the "never raises" checks.
MINIMAL_ARGS = {
    "get_status": {}, "list_formats": {}, "list_documents": {}, "new_document": {},
    "open_document": {"path": "C:/x/a.dxf" if srv.os.name == "nt" else "/x/a.dxf"},
    "activate_document": {"document_id": 1}, "save_document": {},
    "save_document_as": {"path": "C:/x/a.dxf" if srv.os.name == "nt" else "/x/a.dxf"},
    "export_document": {"path": "C:/x/a.png" if srv.os.name == "nt" else "/x/a.png", "format": "png"},
    "close_document": {}, "get_drawing_info": {}, "render_view": {}, "get_view": {},
    "set_view": {"action": "extents"}, "list_layers": {}, "create_layer": {"name": "walls"},
    "update_layer": {"name": "walls", "frozen": True}, "set_current_layer": {"name": "walls"},
    "delete_layer": {"name": "walls", "user_confirmed": True}, "list_blocks": {},
    "insert_block": {"name": "b", "position": [0, 0]}, "query_entities": {}, "get_entity": {"entity_id": 1},
    "add_entities": {"entities": [{"type": "point", "position": [0, 0]}]},
    "update_entity": {"entity_id": 1, "color": "#ff0000"}, "delete_entities": {"entity_ids": [1]},
    "transform_entities": {"entity_ids": [1], "operation": "move", "offset": [1, 1]},
    "select_entities": {"mode": "clear"}, "measure": {"kind": "distance", "points": [[0, 0], [1, 1]]},
    "undo": {}, "redo": {}, "run_command": {"command": "line"}, "list_commands": {},
}


def test_minimal_args_cover_every_tool():
    assert set(MINIMAL_ARGS) == set(srv._TOOL_ANNOTATIONS)


@pytest.mark.parametrize("name", sorted(MINIMAL_ARGS))
async def test_every_tool_answers_an_error_dict_when_librecad_is_not_running(name):
    result = await call_raw(name, MINIMAL_ARGS[name])
    assert not result.is_error
    out = json.loads(result.content[0].text)
    assert "not reachable" in out["error"]


@pytest.mark.parametrize("name", sorted(MINIMAL_ARGS))
async def test_every_tool_explains_a_missing_route(fake, name):
    fake.add_document()
    fake.removed_routes = {r[1] for r in fake.routes if "{" not in r[1]} | {
        "/api/v1/layers/walls", "/api/v1/layers/walls/current", "/api/v1/blocks/b/insert", "/api/v1/entities/1"}
    result = await call_raw(name, MINIMAL_ARGS[name])
    assert not result.is_error
    out = json.loads(result.content[0].text)
    assert out["error"].startswith("not available on this LibreCAD build (needs '"), out


async def test_an_unexpected_exception_is_still_an_error_dict(fake, doc, monkeypatch):
    async def boom(self, *a, **k):
        raise RuntimeError("kaboom")
    monkeypatch.setattr(LibreCadClient, "layers", boom)
    out = await srv.list_layers()
    assert out["error"] == "internal" and "kaboom" in out["detail"]


async def test_client_side_timeout_answers_timed_out(fake, doc):
    doc.path = "C:/x/a.dxf"
    fake.timeout_paths = {"/api/v1/documents/save"}
    out = await srv.save_document()
    assert out["timed_out"] is True and out["error"] == "timed_out"
    assert "list_documents/get_status" in out["hint"]


async def test_server_ui_timeout_keeps_started(fake, doc, monkeypatch):
    import httpx

    def handler(request):
        return httpx.Response(504, json={"error": "ui_timeout", "detail": "LibreCAD did not answer in time",
                                         "started": True})
    cfg = Config("http://127.0.0.1:13140", TOKEN, 30.0, "x")
    monkeypatch.setattr(srv, "_client", lambda: LibreCadClient(cfg, transport=httpx.MockTransport(handler)))
    out = await srv.get_drawing_info()
    assert out["error"] == "ui_timeout" and out["started"] is True and "check with" in out["hint"]


async def test_user_modal_dialog(fake, doc):
    fake.modal_window = "Hatch Settings"
    out = await srv.list_layers()
    assert out == {"error": "gui_modal_open", "window": "Hatch Settings", "hint": out["hint"]}
    assert "close it" in out["hint"]


# --- documents -------------------------------------------------------------------------------------

async def test_document_lifecycle(fake, tmp_path):
    out = await srv.list_documents()
    assert out == {"documents": [], "active": None}
    assert (await srv.get_drawing_info())["error"] == "no_document"
    new = await srv.new_document()
    doc_id = new["document"]["id"]
    second = await srv.new_document(activate=False)
    assert (await srv.list_documents())["active"] == doc_id
    act = await srv.activate_document(second["document"]["id"])
    assert act["document"]["active"] is True
    assert (await srv.activate_document(99))["error"] == "unknown_document"
    unnamed = await srv.save_document()
    assert unnamed["error"] == "document_unnamed" and "save_document_as" in unnamed["hint"]
    info = await srv.get_drawing_info(doc_id)
    assert info["units"] == "mm" and info["counts"]["total"] == 0


async def test_open_document(fake, tmp_path):
    f = tmp_path / "plan.dxf"
    f.write_bytes(b"0\nSECTION\n")
    out = await srv.open_document(str(f))
    assert out["document"]["path"] == str(f) and out["warnings"] == []
    again = await srv.open_document(str(f))
    assert again["already_open"] is True
    missing = await srv.open_document(str(tmp_path / "missing.dxf"))
    assert missing["error"] == "file_not_found" and "absolute path" in missing["hint"]


async def test_open_document_refused_locally_never_reaches_librecad(fake):
    for path, reason in [("plan.dxf", "bad_path"), ("/x/notes.txt" if srv.os.name != "nt" else "C:/x/notes.txt",
                                                    "extension_mismatch")]:
        out = await srv.open_document(path)
        assert out["error"] == "blocked_by_local_policy" and out["reason"] == reason
    assert fake.calls == []


async def test_open_damaged_and_partial_files(fake, tmp_path):
    bad = tmp_path / "bad.dwg"
    bad.write_bytes(b"BAD....")
    assert (await srv.open_document(str(bad)))["error"] == "import_failed"
    part = tmp_path / "part.dwg"
    part.write_bytes(b"PARTIAL")
    out = await srv.open_document(str(part))
    assert out["error"] == "partial_import" and "accept_partial=True" in out["hint"]
    assert "document" in await srv.open_document(str(part), accept_partial=True)


async def test_save_as_and_save(fake, doc, tmp_path):
    target = tmp_path / "plan.dwg"
    out = await srv.save_document_as(str(target), format="dwg", version="2026")
    assert out["path"] == str(target) and out["version"] == "2018" and target.read_bytes() == b"AC1032"
    assert requests(fake, "POST", "/documents/save_as")[-1]["body"] == {
        "path": str(target), "format": "dwg", "version": "2026", "overwrite": False}
    saved = await srv.save_document()
    assert saved["path"] == str(target) and saved["format"] == "dwg" and saved["version"] == "2018"
    assert saved["label"] == out["label"] == "AutoCAD 2018-2026 (AC1032)"
    bad = await srv.save_document_as(str(tmp_path / "x.dxf"), format="dxf", version="R99")
    assert bad["error"] == "bad_param" and "R12" in bad["allowed"]


async def test_export_formats(fake, doc, tmp_path):
    png = await srv.export_document(str(tmp_path / "a.png"), "png", width_px=800, height_px=600)
    assert (tmp_path / "a.png").read_bytes()[:8] == b"\x89PNG\r\n\x1a\n" and png["bytes"] > 0
    body = requests(fake, "POST", "/documents/export")[-1]["body"]
    assert body == {"path": str(tmp_path / "a.png"), "format": "png", "overwrite": False, "width": 800,
                    "height": 600, "margin_px": 16, "background": "white", "monochrome": False}
    await srv.export_document(str(tmp_path / "a.pdf"), "pdf")
    assert (tmp_path / "a.pdf").read_bytes().startswith(b"%PDF")
    assert requests(fake, "POST", "/documents/export")[-1]["body"] == {
        "path": str(tmp_path / "a.pdf"), "format": "pdf", "overwrite": False, "monochrome": False, "fit_to_page": True}
    await srv.export_document(str(tmp_path / "a.svg"), "svg")
    assert b"<svg" in (tmp_path / "a.svg").read_bytes()
    dxf = await srv.export_document(str(tmp_path / "r12.dxf"), "dxf", version="R12")
    assert dxf["version"] == "R12" and doc.path is None  # a copy: the drawing keeps its (lack of) file
    assert dxf["label"] == "DXF R12 (AC1009)"
    assert requests(fake, "POST", "/documents/export")[-1]["body"] == {
        "path": str(tmp_path / "r12.dxf"), "format": "dxf", "overwrite": False, "version": "R12"}


async def test_export_over_the_drawings_own_file_is_refused(fake, doc, tmp_path):
    own = tmp_path / "own.dxf"
    await srv.save_document_as(str(own))
    out = await srv.export_document(str(own), "dxf", overwrite=True, user_confirmed=True)
    assert out["error"] == "file_open_in_other_window" and "save_document" in out["hint"]


async def test_document_id_0_is_a_document(fake):
    """LibreCAD numbers drawings from 0: id 0 must reach it as ?doc=0, not mean "the active one"."""
    first = (await srv.new_document())["document"]["id"]
    await srv.new_document()  # now the active one
    assert first == 0
    info = await srv.get_drawing_info(0)
    assert info["document"]["id"] == 0
    assert requests(fake, "GET", "/drawing")[-1]["query"] == {"doc": ["0"]}


async def test_export_refusals(fake, doc, tmp_path):
    assert (await srv.export_document(str(tmp_path / "a.png"), "png", version="2018"))["error"] == "bad_param"
    out = await srv.export_document(str(tmp_path / "a.pdf"), "png")
    assert out["error"] == "blocked_by_local_policy" and out["reason"] == "extension_mismatch"
    assert requests(fake, "POST", "/documents/export") == []
    fake.capabilities.remove("export_pdf")
    out = await srv.export_document(str(tmp_path / "b.pdf"), "pdf")
    assert out["error"] == "not available on this LibreCAD build (needs 'export_pdf' in get_status capabilities)"
    fake.dwg_support = False
    assert (await srv.export_document(str(tmp_path / "c.dwg"), "dwg"))["error"] == "dwg_unsupported"


async def test_close_document_plain(fake, doc):
    doc.modified = True
    out = await srv.close_document()
    assert out["error"] == "unsaved_changes" and "save_document" in out["hint"]
    doc.modified = False
    assert await srv.close_document() == {"closed": True, "discarded_changes": False}


# --- render and view -------------------------------------------------------------------------------

async def test_render_view_returns_an_image_and_its_window(fake, doc):
    await call("add_entities", {"entities": [{"type": "line", "start": [0, 0], "end": [100, 50]}]})
    out = await srv.render_view(width_px=532, height_px=300)
    assert isinstance(out, list) and isinstance(out[0], Image) and isinstance(out[1], str)
    assert out[0].data == PNG_1X1
    assert out[1].startswith("window [0, 0] to [100, 50] (drawing units); 1 px = 0.2 units (5 px per unit)")
    result = await call_raw("render_view", {})
    assert [c.type for c in result.content] == ["image", "text"]
    assert result.content[0].mime_type == "image/png"


async def test_render_view_modes(fake, doc):
    out = await srv.render_view()
    assert "The drawing is empty" in out[1]
    assert (await srv.render_view(mode="window", window_min=[0, 0]))["error"] == "bad_param"
    assert (await srv.render_view(mode="window", window_min=[5, 5], window_max=[1, 9]))["param"] == "window_max"
    assert (await srv.render_view(mode="extents", window_min=[0, 0]))["error"] == "bad_param"
    out = await srv.render_view(mode="window", window_min=[1.5, -2], window_max=[10, 8], monochrome=True)
    q = requests(fake, "GET", "/render")[-1]["query"]
    assert q["min"] == ["1.5,-2"] and q["max"] == ["10,8"] and q["monochrome"] == ["true"]
    assert out[1].startswith("window [1.5, -2] to [10, 8]")


async def test_views(fake, doc):
    assert (await srv.get_view())["px_per_unit"] == 8.0
    out = await srv.set_view("window", window_min=[0, 0], window_max=[20, 10])
    assert out["window"] == {"min": [0, 0], "max": [20, 10]}
    assert (await srv.set_view("window", window_min=[0, 0]))["error"] == "bad_param"
    assert (await srv.set_view("named"))["param"] == "name"
    assert (await srv.set_view("named", name="front"))["error"] == "unknown_view"


# --- layers and blocks -------------------------------------------------------------------------------

async def test_layers(fake, doc):
    out = await srv.create_layer("Outer walls", color=1, lineweight_mm=0.5, make_current=True)
    assert out["undoable"] is False and out["layer"]["current"] is True
    assert requests(fake, "POST", "/layers")[-1]["body"] == {"name": "Outer walls", "color": 1, "lineweight": 0.5,
                                                            "make_current": True}
    dup = await srv.create_layer("Outer walls")
    assert dup["error"] == "layer_exists" and "hint" in dup
    renamed = await srv.update_layer("Outer walls", new_name="Walls 2", color="#00ff00")
    assert renamed["layer"]["name"] == "Walls 2"
    assert requests(fake, "PATCH")[-1]["path"] == "/api/v1/layers/Outer%20walls"
    assert (await srv.update_layer("Walls 2"))["error"] == "bad_param"
    assert (await srv.update_layer("nope", frozen=True))["error"] == "unknown_layer"
    assert (await srv.set_current_layer("0"))["current"] == "0"
    layers = await srv.list_layers()
    assert [x["name"] for x in layers["layers"]] == ["0", "Walls 2"]


async def test_create_layer_refuses_bylayer_color_before_http(fake, doc):
    result = await call_raw("create_layer", {"name": "x", "color": "ByLayer"})
    assert result.is_error  # the schema allows only #rrggbb or 1..255 for layers
    assert requests(fake, "POST", "/layers") == []


async def test_blocks(fake, doc):
    assert (await srv.list_blocks()) == {"blocks": []}
    out = await srv.insert_block("door", [0, 0])
    assert out["error"] == "unknown_block" and "list_blocks" in out["hint"]
    doc.blocks["door"] = {"name": "door", "base_point": [0, 0], "entity_count": 3, "frozen": False}
    out = await srv.insert_block("door", [5, 5], scale=[2, 1], angle_deg=90)
    assert out["entity"]["type"] == "insert"
    assert requests(fake, "POST", "/blocks/door/insert")[-1]["body"] == {
        "position": [5, 5], "scale": [2, 1], "angle_deg": 90, "select": False}
    assert (await srv.list_blocks())["blocks"][0]["insert_count"] == 1


# --- entities ----------------------------------------------------------------------------------------

async def test_add_entities_is_one_atomic_undo_step(fake, doc):
    await srv.create_layer("walls")
    specs = [
        {"type": "line", "start": [0, 0], "end": [100, 0], "color": "#ff0000", "lineweight": 0.35},
        {"type": "circle", "center": [50, 50], "radius": 10, "layer": "0"},
        {"type": "arc", "center": [0, 0], "radius": 5, "start_angle_deg": 0, "end_angle_deg": 90},
        {"type": "ellipse", "center": [0, 0], "major_axis": [10, 0], "ratio": 0.5},
        {"type": "point", "position": [1, 1]},
        SQUARE,
        {"type": "spline", "fit_points": [[0, 0], [5, 5], [10, 0]]},
        {"type": "spline", "control_points": [[0, 0], [5, 5], [10, 0]], "degree": 2},
        {"type": "text", "position": [0, -10], "text": "Plan", "height": 2.5, "halign": "center"},
        {"type": "mtext", "position": [0, -20], "text": "A\\PB", "height": 2, "attachment": "middle_center"},
        {"type": "dimension", "dim_type": "linear", "p1": [0, 0], "p2": [100, 0], "line_point": [50, -5]},
        {"type": "dimension", "dim_type": "radial", "center": [50, 50], "point": [60, 50]},
        {"type": "hatch", "pattern": "SOLID", "boundary": [[[0, 0], [5, 0], [5, 5]]]},
        {"type": "hatch", "boundary": [{"vertices": [[0, 0], [5, 0, 1]], "closed": True}], "pattern": "ANSI31"},
    ]
    out = await call("add_entities", {"entities": specs, "layer": "walls"})
    assert out["undoable"] is True and len(out["ids"]) == len(specs) == len(set(out["ids"]))
    sent = requests(fake, "POST", "/entities")[-1]["body"]
    assert sent["layer"] == "walls" and sent["select"] is False
    # only the fields the caller set are sent; LibreCAD applies the defaults
    assert sent["entities"][0] == specs[0]
    assert sent["entities"][7] == specs[7] and sent["entities"][12] == specs[12]
    assert [e["layer"] for e in out["created"][:2]] == ["walls", "0"]
    assert len(doc.entities) == len(specs)
    undone = await srv.undo()
    assert undone["undone"] == 1 and len(doc.entities) == 0
    assert (await srv.redo())["redone"] == 1 and len(doc.entities) == len(specs)


@pytest.mark.parametrize("spec", [
    {"type": "circle", "center": [0, 0], "radius": 0},
    {"type": "circle", "center": [0, 0]},
    {"type": "circle", "center": [0], "radius": 1},
    {"type": "triangle", "points": [[0, 0]]},
    {"type": "line", "start": [0, 0], "end": [1, 1], "radius": 3},
    {"type": "polyline", "vertices": [[0, 0]]},
    {"type": "spline", "control_points": [[0, 0], [1, 1]], "degree": 3},
    {"type": "spline", "control_points": [[0, 0], [1, 1]], "fit_points": [[0, 0], [1, 1]]},
    {"type": "spline", "fit_points": [[0, 0], [1, 1]], "degree": 2},
    {"type": "dimension", "dim_type": "linear", "p1": [0, 0], "line_point": [0, 5]},
    {"type": "dimension", "dim_type": "radial", "center": [0, 0], "point": [1, 0], "p1": [0, 0]},
    {"type": "hatch", "boundary": [[[0, 0], [1, 0], [1, 1]]], "boundary_ids": [1]},
    {"type": "line", "start": [0, 0], "end": [1, 1], "color": 300},
    {"type": "line", "start": [0, 0], "end": [1, 1], "color": "red"},
    {"type": "line", "start": [0, 0], "end": [1, 1], "lineweight": 5},
    {"type": "ellipse", "center": [0, 0], "major_axis": [1, 0], "ratio": 1.5},
    {"type": "text", "position": [0, 0], "text": "", "height": 1},
])
async def test_bad_specs_are_rejected_before_any_http(fake, doc, spec):
    result = await call_raw("add_entities", {"entities": [{"type": "point", "position": [0, 0]}, spec]})
    assert result.is_error
    assert requests(fake, "POST", "/entities") == []


async def test_add_entities_reports_librecads_own_refusal(fake, doc):
    out = await call("add_entities", {"entities": [
        {"type": "point", "position": [0, 0]},
        {"type": "text", "position": [0, 0], "text": "x", "height": 1, "style": "gothic"}]})
    assert out["error"] == "invalid_entity" and out["errors"] == {"entities[1].style": "unknown text style"}
    assert "Nothing was applied" in out["message"] and "all-or-nothing" in out["hint"]
    assert len(doc.entities) == 0
    out = await call("add_entities", {"entities": [{"type": "point", "position": [0, 0]}], "layer": "nope"})
    assert out["error"] == "unknown_layer" and out["name"] == "nope" and "list_layers" in out["hint"]
    await srv.create_layer("locked", locked=True)
    out = await call("add_entities", {"entities": [{"type": "point", "position": [0, 0], "layer": "locked"}]})
    assert out["error"] == "layer_locked" and out["layer"] == "locked"


async def test_an_insert_of_a_missing_block_creates_nothing(fake, doc):
    out = await call("add_entities", {"entities": [
        {"type": "line", "start": [0, 0], "end": [1, 1]},
        {"type": "insert", "block": "ghost", "position": [0, 0]}]})
    assert out["error"] == "unknown_block" and out["name"] == "ghost" and "list_blocks" in out["hint"]
    assert len(doc.entities) == 0


async def test_dimensions_report_their_measurement(fake, doc):
    out = await call("add_entities", {"entities": [
        {"type": "dimension", "dim_type": "linear", "p1": [0, 0], "p2": [30, 40], "line_point": [0, -5]},
        {"type": "dimension", "dim_type": "aligned", "p1": [0, 0], "p2": [30, 40], "line_point": [0, 5]}]})
    assert [round(e["measurement"], 6) for e in out["created"]] == [30, 50]


async def test_text_second_point(fake, doc):
    aligned = {"type": "text", "position": [0, 0], "text": "x", "height": 1, "halign": "aligned"}
    result = await call_raw("add_entities", {"entities": [aligned]})
    assert result.is_error and "second_point" in result.content[0].text
    assert requests(fake, "POST", "/entities") == []
    out = await call("add_entities", {"entities": [{**aligned, "second_point": [10, 0]}]})
    assert out["created"][0]["second_point"] == [10, 0]
    assert requests(fake, "POST", "/entities")[-1]["body"]["entities"][0]["second_point"] == [10, 0]


async def test_invalid_entity_passes_on_the_allowed_values(fake, doc):
    out = await call("add_entities", {"entities": [
        {"type": "point", "position": [0, 0]},
        {"type": "line", "start": [0, 0], "end": [1, 1], "lineweight": 0.33}]})
    assert out["error"] == "invalid_entity" and list(out["errors"]) == ["entities[1].lineweight"]
    assert 0.35 in out["allowed"]["entities[1].lineweight"] and "allowed" in out["hint"]
    assert len(doc.entities) == 0


async def test_query_and_get_entities(fake, doc):
    await srv.create_layer("walls")
    created = await call("add_entities", {"entities": [
        {"type": "line", "start": [0, 0], "end": [10, 0], "layer": "walls"},
        {"type": "line", "start": [20, 20], "end": [30, 30]},
        {"type": "circle", "center": [5, 5], "radius": 1}]})
    ids = created["ids"]
    out = await srv.query_entities(types=["line", "arc"], layers=["walls"])
    assert [e["id"] for e in out["entities"]] == [ids[0]]
    q = requests(fake, "GET", "/entities")[-1]["query"]
    assert q["type"] == ["line", "arc"] and q["layer"] == ["walls"] and "selected" not in q
    page = await srv.query_entities(limit=2)
    assert page["next_offset"] == 2 and page["total"] == 3
    inside = await srv.query_entities(bbox_min=[-1, -1], bbox_max=[11, 11], bbox_mode="window")
    assert sorted(e["id"] for e in inside["entities"]) == [ids[0], ids[2]]
    assert requests(fake, "GET", "/entities")[-1]["query"]["bbox"] == ["-1,-1,11,11"]
    assert (await srv.query_entities(bbox_min=[0, 0]))["error"] == "bad_param"
    one = await srv.get_entity(ids[2])
    assert one["entity"]["radius"] == 1
    missing = await srv.get_entity(424242)
    assert missing["error"] == "unknown_entity" and missing["ids"] == [424242] and "query_entities" in missing["hint"]


async def test_update_entity_keeps_the_id(fake, doc):
    eid = (await call("add_entities", {"entities": [{"type": "circle", "center": [0, 0], "radius": 1}]}))["ids"][0]
    out = await srv.update_entity(eid, color="#00ff00", lineweight_mm="ByLayer", geometry={"radius": 5})
    assert out["entity"]["id"] == eid and out["entity"]["radius"] == 5 and out["entity"]["color"] == "#00ff00"
    assert requests(fake, "PATCH")[-1]["body"] == {"radius": 5, "color": "#00ff00", "lineweight": "ByLayer"}
    assert (await srv.update_entity(eid))["error"] == "bad_param"
    wrong = await srv.update_entity(eid, geometry={"end": [1, 1]})
    assert wrong["error"] == "unknown_field" and wrong["field"] == "end"
    assert (await srv.update_entity(eid, geometry={"type": "line"}))["error"] == "bad_param"


async def test_delete_and_undo_restores_the_same_ids(fake, doc):
    ids = (await call("add_entities", {"entities": [{"type": "point", "position": [0, 0]},
                                                    {"type": "point", "position": [1, 1]}]}))["ids"]
    missing = await srv.delete_entities([ids[0], 999])
    assert missing["error"] == "unknown_entity" and missing["ids"] == [999] and len(doc.entities) == 2
    out = await srv.delete_entities(ids)
    assert out == {"deleted": ids, "undoable": True} and not doc.entities
    await srv.undo()
    assert sorted(doc.entities) == sorted(ids)


async def test_transform_entities(fake, doc):
    eid = (await call("add_entities", {"entities": [{"type": "line", "start": [0, 0], "end": [10, 0]}]}))["ids"][0]
    moved = await srv.transform_entities([eid], "move", offset=[5, 5])
    assert moved["ids"] == [eid] and doc.entities[eid]["start"] == [5, 5]
    assert requests(fake, "POST", "/entities/transform")[-1]["body"] == {
        "ids": [eid], "op": "move", "copy": False, "offset": [5, 5]}
    copies = await srv.transform_entities([eid], "move", offset=[0, 10], copy=True, copies=3)
    assert len(copies["ids"]) == 3 and eid not in copies["ids"] and copies["originals_kept"] is True
    assert len(doc.entities) == 4
    await srv.transform_entities([eid], "rotate", center=[5, 5], angle_deg=90)
    assert [round(v, 9) for v in doc.entities[eid]["end"]] == [5, 15]
    await srv.transform_entities([eid], "mirror", axis_start=[0, 0], axis_end=[0, 1])
    assert requests(fake, "POST", "/entities/transform")[-1]["body"]["axis"] == [[0, 0], [0, 1]]
    await srv.transform_entities([eid], "scale", base=[0, 0], factor=[2, 1])
    assert requests(fake, "POST", "/entities/transform")[-1]["body"]["factor"] == [2, 1]


@pytest.mark.parametrize("kwargs,param", [
    ({"operation": "move"}, "offset"),
    ({"operation": "rotate", "center": [0, 0]}, "angle_deg"),
    ({"operation": "scale", "factor": 2}, "base"),
    ({"operation": "mirror", "axis_start": [0, 0]}, "axis_end"),
    ({"operation": "move", "offset": [1, 1], "angle_deg": 5}, "angle_deg"),
    ({"operation": "move", "offset": [1, 1], "copies": 3}, "copies"),
])
async def test_transform_parameter_checks(fake, doc, kwargs, param):
    out = await srv.transform_entities([1], **kwargs)
    assert out["error"] == "bad_param" and out["param"] == param
    assert requests(fake, "POST", "/entities/transform") == []


async def test_locked_entities_are_not_transformed(fake, doc):
    await srv.create_layer("frozen-in-time")
    eid = (await call("add_entities", {"entities": [{"type": "point", "position": [0, 0], "layer": "frozen-in-time"}]}))["ids"][0]
    await srv.update_layer("frozen-in-time", locked=True)
    out = await srv.transform_entities([eid], "move", offset=[1, 0])
    assert out["error"] == "entity_locked" and out["ids"] == [eid] and "ask the user" in out["hint"]
    assert len(doc.entities) == 1


async def test_selection(fake, doc):
    ids = (await call("add_entities", {"entities": [{"type": "point", "position": [0, 0]},
                                                    {"type": "circle", "center": [0, 0], "radius": 1}]}))["ids"]
    assert (await srv.select_entities("set", entity_ids=[ids[0]]))["ids"] == [ids[0]]
    assert (await srv.select_entities("add", types=["circle"]))["count"] == 2
    q = await srv.query_entities(selected_only=True)
    assert requests(fake, "GET", "/entities")[-1]["query"]["selected"] == ["true"] and q["total"] == 2
    await srv.select_entities("set", bbox_min=[0, 0], bbox_max=[1, 1], bbox_mode="window")
    assert requests(fake, "POST", "/selection")[-1]["body"] == {
        "mode": "set", "bbox": [0, 0, 1, 1], "bbox_mode": "window"}
    assert (await srv.select_entities("clear"))["count"] == 0


async def test_measure(fake, doc):
    assert (await srv.measure("distance", points=[[0, 0], [3, 4]]))["distance"] == 5
    sq = (await call("add_entities", {"entities": [SQUARE]}))["ids"][0]
    area = await srv.measure("area", entity_ids=[sq])
    assert area["area"] == 100 and area["perimeter"] == 40
    assert (await srv.measure("area", points=[[0, 0], [4, 0], [4, 3]]))["area"] == 6
    lines = (await call("add_entities", {"entities": [
        {"type": "line", "start": [0, 0], "end": [10, 10]},
        {"type": "line", "start": [0, 10], "end": [10, 0]},
        {"type": "polyline", "vertices": [[0, 0], [1, 1]]}]}))["ids"]
    assert (await srv.measure("intersections", entity_ids=lines[:2]))["points"] == [[5, 5]]
    open_area = await srv.measure("area", entity_ids=[lines[2]])
    assert open_area["error"] == "not_closed" and open_area["ids"] == [lines[2]] and "closed" in open_area["hint"]
    assert (await srv.measure("angle", points=[[0, 0], [1, 0], [0, 1]]))["angle_deg"] == 90
    assert (await srv.measure("length", entity_ids=[lines[0]]))["length"] > 14
    assert requests(fake, "POST", "/measure")[0]["body"] == {"kind": "distance", "from": [0, 0], "to": [3, 4]}


@pytest.mark.parametrize("kwargs", [
    {"kind": "distance", "points": [[0, 0]]},
    {"kind": "length", "points": [[0, 0]]},
    {"kind": "area", "points": [[0, 0], [1, 1]]},
    {"kind": "angle", "entity_ids": [1]},
    {"kind": "intersections", "entity_ids": [1, 2, 3]},
])
async def test_measure_parameter_checks(fake, doc, kwargs):
    assert (await srv.measure(**kwargs))["error"] == "bad_param"
    assert requests(fake, "POST", "/measure") == []


# --- command line ------------------------------------------------------------------------------------

async def test_run_command(fake, doc):
    out = await srv.run_command("line", ["0,0", "@100,0", "escape"])
    assert out["executed"] == ["line", "0,0", "@100,0", "escape"] and len(doc.entities) == 1
    assert next(iter(doc.entities.values()))["end"] == [100, 0]
    unknown = await srv.run_command("frobnicate")
    assert unknown["unknown_commands"] == ["frobnicate"]
    assert (await srv.run_command("line", ["0,0", " "]))["param"] == "inputs[1]"
    assert (await srv.run_command("line", ["0,0\n1,1"]))["error"] == "bad_param"
    assert (await srv.run_command("   "))["error"] == "bad_param"
    # /commands lists the main names only ("line2p"); aliases such as "line" or "li" still run
    commands = (await srv.list_commands())["commands"]
    assert "line2p" in commands and "line" not in commands
    assert (await srv.run_command("li", ["0,0", "1,1", "escape"]))["unknown_commands"] == []
    fake.allow_commands = False
    off = await srv.run_command("line")
    assert off["error"] == "commands_disabled" and "Preferences" in off["hint"]


async def test_formats_and_routes(fake):
    out = await srv.list_formats()
    assert out["save"][1]["labels"]["2018"] == "AutoCAD 2018-2026 (AC1032)"
    dxf, dwg = out["save"]
    assert dxf["extension"] == ".dxf" and dwg["extension"] == ".dwg"
    assert set(dxf["labels"]) == set(dxf["versions"]) and dxf["labels"]["R12"] == "DXF R12 (AC1009)"
    assert dwg["aliases"]["2026"] == dwg["aliases"]["2019"] == dwg["aliases"]["AC1032"] == "2018"
    assert dwg["aliases"]["R2000"] == "2000" and dxf["aliases"]["AC1009"] == "R12"
