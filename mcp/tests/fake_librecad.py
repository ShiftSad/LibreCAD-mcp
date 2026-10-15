# SPDX-License-Identifier: GPL-2.0-or-later
"""A stateful in-memory fake of LibreCAD's Remote API v1 (DESIGN.md section 2), served through
httpx.MockTransport.

It keeps documents, layers, blocks, entities with an id counter, an undo/redo stack, the
selection and the on-screen view, writes real files for save/export (so path and overwrite
logic can be tested against a temp folder), and answers with the contract's error tokens:
bare 404 not_found for unknown routes, 405 with allowed, 401 with app, 400 unknown_field,
404 unknown_entity/unknown_layer/file_not_found, 409 file_exists/layer_not_empty/unsaved_changes,
422 invalid_entity with errors, and so on.
"""
from __future__ import annotations

import copy
import json
import math
import os
import re
import struct
import zlib
from typing import Any
from urllib.parse import unquote

import httpx


def tiny_png(width: int = 1, height: int = 1) -> bytes:
    """A valid white PNG of the given size (stdlib only)."""
    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + b"\xff\xff\xff" * width for _ in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


PNG_1X1 = tiny_png()

OPEN_EXT = (".dxf", ".dwg", ".lff", ".cxf", ".jww", ".jwc", ".shp")
DXF_VERSIONS = ["R12", "R14", "2000", "2004", "2007", "2010", "2013", "2018"]
DWG_VERSIONS = ["2000", "2004", "2007", "2010", "2013", "2018"]
DWG_ALIASES = {"2026": "2018", "AC1032": "2018", "R2000": "2000"}
EXT = {"dxf": ".dxf", "dwg": ".dwg", "pdf": ".pdf", "png": ".png", "svg": ".svg"}
CAPABILITIES = ["status", "routes", "documents", "export", "export_dwg", "export_pdf", "drawing", "render",
                "view", "undo", "command", "layers", "blocks", "entities", "transform", "selection", "measure"]
ATTRS = {"layer", "color", "linetype", "lineweight"}
TYPE_FIELDS = {
    "line": {"start", "end"},
    "circle": {"center", "radius"},
    "arc": {"center", "radius", "start_angle_deg", "end_angle_deg", "reversed"},
    "ellipse": {"center", "major_axis", "ratio", "start_angle_deg", "end_angle_deg", "reversed"},
    "point": {"position"},
    "polyline": {"vertices", "closed"},
    "spline": {"control_points", "degree", "fit_points", "closed"},
    "text": {"position", "text", "height", "angle_deg", "halign", "valign", "style", "width_factor"},
    "mtext": {"position", "text", "height", "width", "angle_deg", "attachment", "line_spacing", "style"},
    "dimension": {"dim_type", "p1", "p2", "line_point", "angle_deg", "center", "point", "leader_length",
                  "opposite_point", "line1", "line2", "arc_point", "text", "style"},
    "hatch": {"pattern", "scale", "angle_deg", "boundary", "boundary_ids"},
    "insert": {"block", "position", "scale", "angle_deg", "columns", "rows", "spacing"},
}
POINT_KEYS = ("start", "end", "center", "position", "p1", "p2", "line_point", "point", "opposite_point",
              "arc_point")
POINT_LIST_KEYS = ("vertices", "control_points", "fit_points")
LAYER_NAME_BAD = re.compile(r'[<>/\\":;?*|,=`]')
COMMANDS = ["line", "circle", "arc", "polyline", "zoomauto", "zoomin", "select", "move", "escape"]


class FakeError(Exception):
    def __init__(self, status: int, token: str, detail: str = "", **extra):
        super().__init__(token)
        self.status, self.token, self.detail, self.extra = status, token, detail, extra

    def response(self) -> httpx.Response:
        body = {"error": self.token}
        if self.detail:
            body["detail"] = self.detail
        body.update(self.extra)
        return httpx.Response(self.status, json=body)


def allow_only(body: dict, keys) -> None:
    for k in body:
        if k not in keys:
            raise FakeError(400, "unknown_field", f"unknown field '{k}'", field=k)


def need(body: dict, key: str):
    if key not in body:
        raise FakeError(400, "missing_param", f"'{key}' is required", param=key)
    return body[key]


class Doc:
    def __init__(self, doc_id: int, name: str, path: str | None = None, fmt: str | None = None):
        self.id, self.name, self.path, self.fmt = doc_id, name, path, fmt
        self.modified = False
        self.entities: dict[int, dict] = {}
        self.layers: dict[str, dict] = {"0": self._layer("0")}
        self.current_layer = "0"
        self.blocks: dict[str, dict] = {}
        self.undo_stack: list[tuple] = []
        self.redo_stack: list[tuple] = []
        self.selection: set[int] = set()
        self.view = {"window": {"min": [0.0, 0.0], "max": [100.0, 75.0]}, "center": [50.0, 37.5],
                     "size_px": [800, 600], "px_per_unit": 8.0}
        self.history: list[str] = []

    @staticmethod
    def _layer(name: str, color: str = "#ffffff", aci: int | None = 7) -> dict:
        return {"name": name, "color": color, "aci": aci, "linetype": "Continuous", "lineweight": "Default",
                "frozen": False, "locked": False, "printable": True, "construction": False}

    def snapshot(self) -> None:
        self.undo_stack.append((copy.deepcopy(self.entities), set(self.selection)))
        self.redo_stack.clear()
        self.modified = True


class FakeLibreCAD:
    def __init__(self, token: str = "tok"):
        self.token = token
        self.calls: list[dict] = []
        self.docs: dict[int, Doc] = {}
        self.active: int | None = None
        self.next_doc = 1
        self.next_entity = 100
        self.allow_commands = True
        self.dwg_support = True
        self.capabilities = list(CAPABILITIES)
        self.modal_window: str | None = None   # the user has a modal dialog open
        self.timeout_paths: set[str] = set()   # paths whose request times out (client side)
        self.removed_routes: set[str] = set()  # simulate an older build: these paths answer bare 404
        self.routes = self._route_table()

    # --- plumbing ------------------------------------------------------------------------------

    def transport(self) -> httpx.MockTransport:
        return httpx.MockTransport(self.handle)

    def _route_table(self):
        R = []

        def add(method, pattern, fn, cap, changes=False):
            regex = "^" + re.sub(r"\{(\w+)\}", r"(?P<\1>[^/]+)", pattern) + "$"
            R.append((method, pattern, re.compile(regex), fn, cap, changes))
        add("GET", "/api/v1/status", self.h_status, "status")
        add("GET", "/api/v1/routes", self.h_routes, "routes")
        add("GET", "/api/v1/formats", self.h_formats, "documents")
        add("GET", "/api/v1/documents", self.h_documents, "documents")
        add("POST", "/api/v1/documents/new", self.h_new, "documents", True)
        add("POST", "/api/v1/documents/open", self.h_open, "documents", True)
        add("POST", "/api/v1/documents/activate", self.h_activate, "documents")
        add("POST", "/api/v1/documents/save", self.h_save, "documents", True)
        add("POST", "/api/v1/documents/save_as", self.h_save_as, "documents", True)
        add("POST", "/api/v1/documents/export", self.h_export, "export")
        add("POST", "/api/v1/documents/close", self.h_close, "documents", True)
        add("GET", "/api/v1/drawing", self.h_drawing, "drawing")
        add("GET", "/api/v1/render", self.h_render, "render")
        add("GET", "/api/v1/view", self.h_get_view, "view")
        add("POST", "/api/v1/view", self.h_set_view, "view")
        add("POST", "/api/v1/undo", self.h_undo, "undo", True)
        add("POST", "/api/v1/redo", self.h_redo, "undo", True)
        add("POST", "/api/v1/command", self.h_command, "command", True)
        add("GET", "/api/v1/commands", self.h_commands, "command")
        add("GET", "/api/v1/layers", self.h_layers, "layers")
        add("POST", "/api/v1/layers", self.h_create_layer, "layers", True)
        add("PATCH", "/api/v1/layers/{name}", self.h_update_layer, "layers", True)
        add("POST", "/api/v1/layers/{name}/current", self.h_current_layer, "layers")
        add("DELETE", "/api/v1/layers/{name}", self.h_delete_layer, "layers", True)
        add("GET", "/api/v1/blocks", self.h_blocks, "blocks")
        add("POST", "/api/v1/blocks/{name}/insert", self.h_insert_block, "blocks", True)
        add("GET", "/api/v1/entities", self.h_entities, "entities")
        add("GET", "/api/v1/entities/{id}", self.h_entity, "entities")
        add("POST", "/api/v1/entities", self.h_create, "entities", True)
        add("PATCH", "/api/v1/entities/{id}", self.h_update, "entities", True)
        add("DELETE", "/api/v1/entities/{id}", self.h_delete_one, "entities", True)
        add("POST", "/api/v1/entities/delete", self.h_delete, "entities", True)
        add("POST", "/api/v1/entities/transform", self.h_transform, "transform", True)
        add("GET", "/api/v1/selection", self.h_get_selection, "selection")
        add("POST", "/api/v1/selection", self.h_set_selection, "selection")
        add("POST", "/api/v1/measure", self.h_measure, "measure")
        return R

    def handle(self, request: httpx.Request) -> httpx.Response:
        # Route on the raw, still percent-encoded path, and decode each {param} once, as the app does.
        path = request.url.raw_path.decode("ascii").split("?", 1)[0]
        query: dict[str, list[str]] = {}
        for k, v in request.url.params.multi_items():
            query.setdefault(k, []).append(v)
        raw = request.content
        self.calls.append({"method": request.method, "path": path, "query": query,
                           "body": json.loads(raw) if raw else None, "headers": dict(request.headers)})
        if path in self.timeout_paths:
            raise httpx.ReadTimeout("timed out", request=request)
        if "origin" in request.headers:
            return FakeError(403, "origin_not_allowed").response()
        auth = request.headers.get("x-api-token") or ""
        bearer = request.headers.get("authorization", "")
        if bearer.lower().startswith("bearer "):
            auth = auth or bearer[7:]
        if auth != self.token:
            return httpx.Response(401, json={"error": "unauthorized", "app": "LibreCAD"})
        if path in self.removed_routes:
            return FakeError(404, "not_found").response()
        allowed = []
        for method, _pattern, regex, fn, _cap, _changes in self.routes:
            m = regex.match(path)
            if not m:
                continue
            allowed.append(method)
            if method != request.method:
                continue
            params = {k: unquote(v) for k, v in m.groupdict().items()}
            try:
                body: dict = {}
                if raw:
                    try:
                        body = json.loads(raw)
                    except ValueError:
                        raise FakeError(400, "invalid_json") from None
                    if not isinstance(body, dict):
                        raise FakeError(400, "body_must_be_object")
                if self.modal_window and path != "/api/v1/status":
                    raise FakeError(409, "gui_modal_open", window=self.modal_window)
                out = fn(query, body, **params)
            except FakeError as e:
                return e.response()
            if isinstance(out, httpx.Response):
                return out
            status, data = out if isinstance(out, tuple) else (200, out)
            return httpx.Response(status, json=data)
        if allowed:
            return FakeError(405, "method_not_allowed", allowed=sorted(set(allowed))).response()
        return FakeError(404, "not_found").response()

    # --- documents helpers -----------------------------------------------------------------------

    def add_document(self, name: str | None = None, path: str | None = None, activate: bool = True) -> Doc:
        doc = Doc(self.next_doc, name or f"Unnamed {self.next_doc}", path,
                  ("dwg" if path and path.lower().endswith(".dwg") else "dxf") if path else None)
        self.docs[doc.id] = doc
        self.next_doc += 1
        if activate or self.active is None:
            self.active = doc.id
        return doc

    def doc(self, query: dict) -> Doc:
        if "doc" in query:
            try:
                doc_id = int(query["doc"][0])
            except ValueError:
                raise FakeError(400, "bad_param", "doc must be an integer", param="doc") from None
            if doc_id not in self.docs:
                raise FakeError(404, "unknown_document", doc=doc_id)
            return self.docs[doc_id]
        if self.active is None:
            raise FakeError(409, "no_document", "no drawing is open")
        return self.docs[self.active]

    def doc_json(self, d: Doc) -> dict:
        fmt = None
        if d.fmt:
            fmt = {"format": d.fmt, "version": "2018", "label": "AutoCAD 2018-2026 (AC1032)" if d.fmt == "dwg" else "DXF 2018"}
        return {"id": d.id, "name": d.name, "path": d.path, "modified": d.modified, "active": d.id == self.active,
                "format": fmt, "entity_count": len(d.entities)}

    # --- meta ----------------------------------------------------------------------------------------

    def h_status(self, q, body):
        return {"app": "LibreCAD", "app_version": "2.2.2.6-alpha", "mcp_build": 1, "api_version": "1.0",
                "platform": "windows", "capabilities": self.capabilities, "dwg_support": self.dwg_support,
                "commands_allowed": self.allow_commands, "discovery_file": "C:/fake/remote-api.json",
                "documents": [{k: v for k, v in self.doc_json(d).items() if k in ("id", "name", "path", "modified", "active")}
                              for d in self.docs.values()],
                "active_document": self.active,
                "gui": {"modal_open": self.modal_window is not None, "active_action": None}}

    def h_routes(self, q, body):
        return {"api_version": "1.0", "routes": [
            {"method": m, "path": p, "capability": cap, "changes_document": ch, "timeout_s": 10, "summary": ""}
            for m, p, _r, _f, cap, ch in self.routes]}

    def h_formats(self, q, body):
        return {"open": list(OPEN_EXT),
                "save": [{"format": "dxf", "versions": DXF_VERSIONS, "default": "2007"},
                         {"format": "dwg", "versions": DWG_VERSIONS, "default": "2018",
                          "labels": {"2018": "AutoCAD 2018-2026 (AC1032)"}, "aliases": DWG_ALIASES}],
                "export": ["dxf", "dwg", "pdf", "png", "svg"]}

    def h_documents(self, q, body):
        return {"documents": [self.doc_json(d) for d in self.docs.values()], "active": self.active}

    def h_new(self, q, body):
        allow_only(body, {"activate"})
        d = self.add_document(activate=body.get("activate", True))
        return 201, {"document": self.doc_json(d)}

    def _check_abs(self, path):
        if not isinstance(path, str) or not os.path.isabs(path):
            raise FakeError(400, "bad_path", "the path must be absolute", path=path)

    def h_open(self, q, body):
        allow_only(body, {"path", "accept_partial"})
        path = need(body, "path")
        self._check_abs(path)
        for d in self.docs.values():
            if d.path and os.path.normcase(d.path) == os.path.normcase(path):
                self.active = d.id
                return {"document": self.doc_json(d), "already_open": True}
        if not os.path.isfile(path):
            raise FakeError(404, "file_not_found", path=path)
        if not path.lower().endswith(OPEN_EXT):
            raise FakeError(422, "unsupported_format", allowed=list(OPEN_EXT))
        with open(path, "rb") as fh:
            head = fh.read(16)
        if head.startswith(b"BAD"):
            raise FakeError(422, "import_failed", detail="the file is damaged")
        if head.startswith(b"PARTIAL") and not body.get("accept_partial", False):
            raise FakeError(409, "partial_import", detail="3 objects could not be read")
        d = self.add_document(os.path.basename(path), path)
        return 201, {"document": self.doc_json(d), "warnings": []}

    def h_activate(self, q, body):
        d = self.doc(q)
        self.active = d.id
        return {"document": self.doc_json(d)}

    def h_save(self, q, body):
        allow_only(body, set())
        d = self.doc(q)
        if not d.path:
            raise FakeError(409, "document_unnamed", hint="use save_as")
        with open(d.path, "wb") as fh:
            fh.write(b"0\nSECTION\n")
        d.modified = False
        return {"document": self.doc_json(d), "path": d.path, "format": d.fmt or "dxf", "left_out": []}

    def _write_target(self, body, formats, versions_for):
        path = need(body, "path")
        fmt = need(body, "format")
        self._check_abs(path)
        if fmt not in formats:
            raise FakeError(422, "unsupported_format", allowed=formats)
        if not path.lower().endswith(EXT[fmt]):
            raise FakeError(422, "extension_mismatch", f"a {fmt} file must end in {EXT[fmt]}")
        if fmt == "dwg" and not self.dwg_support:
            raise FakeError(501, "dwg_unsupported")
        version = body.get("version")
        if version is not None and fmt in versions_for:
            version = DWG_ALIASES.get(version, version) if fmt == "dwg" else version
            if version not in versions_for[fmt]:
                raise FakeError(422, "bad_param", "unknown version", param="version", allowed=versions_for[fmt])
        if not os.path.isdir(os.path.dirname(path)):
            raise FakeError(400, "bad_path", "the folder does not exist", path=path)
        if os.path.exists(path) and not body.get("overwrite", False):
            raise FakeError(409, "file_exists", path=path)
        return path, fmt, version

    def h_save_as(self, q, body):
        allow_only(body, {"path", "format", "version", "overwrite"})
        d = self.doc(q)
        path, fmt, version = self._write_target(body, ["dxf", "dwg"], {"dxf": DXF_VERSIONS, "dwg": DWG_VERSIONS})
        with open(path, "wb") as fh:
            fh.write(b"AC1032" if fmt == "dwg" else b"0\nSECTION\n")
        d.path, d.name, d.fmt, d.modified = path, os.path.basename(path), fmt, False
        return {"document": self.doc_json(d), "path": path, "format": fmt,
                "version": version or ("2018" if fmt == "dwg" else "2007"), "left_out": []}

    def h_export(self, q, body):
        allow_only(body, {"path", "format", "version", "overwrite", "width", "height", "margin_px",
                          "background", "monochrome", "fit_to_page"})
        d = self.doc(q)
        path, fmt, version = self._write_target(body, ["dxf", "dwg", "pdf", "png", "svg"],
                                                {"dxf": DXF_VERSIONS, "dwg": DWG_VERSIONS})
        data = {"pdf": b"%PDF-1.4\n%fake\n", "png": PNG_1X1, "svg": b"<svg xmlns='http://www.w3.org/2000/svg'/>",
                "dxf": b"0\nSECTION\n", "dwg": b"AC1032"}[fmt]
        with open(path, "wb") as fh:
            fh.write(data)
        out = {"path": path, "format": fmt, "bytes": len(data), "left_out": []}
        if fmt in ("dxf", "dwg"):
            out["version"] = version or ("2018" if fmt == "dwg" else "2007")
        _ = d
        return out

    def h_close(self, q, body):
        allow_only(body, {"discard_changes"})
        d = self.doc(q)
        discard = body.get("discard_changes", False)
        if d.modified and not discard:
            raise FakeError(409, "unsaved_changes", f"'{d.name}' has unsaved changes")
        del self.docs[d.id]
        if self.active == d.id:
            self.active = next(iter(self.docs), None)
        return {"closed": True, "discarded_changes": bool(d.modified and discard)}

    def h_drawing(self, q, body):
        d = self.doc(q)
        counts: dict[str, int] = {}
        for e in d.entities.values():
            counts[e["type"]] = counts.get(e["type"], 0) + 1
        ext = self._extents(d.entities.values())
        return {"document": self.doc_json(d), "units": "mm", "unit_code": 4, "extents": ext, "extents_all": ext,
                "format": self.doc_json(d)["format"], "counts": {"total": len(d.entities), "by_type": counts},
                "layers": len(d.layers), "blocks": len(d.blocks), "current_layer": d.current_layer,
                "undo": {"can_undo": bool(d.undo_stack), "can_redo": bool(d.redo_stack)},
                "variables": {"$INSUNITS": 4, "$LTSCALE": 1.0, "$DIMSCALE": 1.0}, "paper_space": "not_supported"}

    # --- view and render ------------------------------------------------------------------------

    def h_render(self, q, body):
        d = self.doc(q)
        mode = (q.get("mode") or ["extents"])[0]
        if mode not in ("extents", "view", "window"):
            raise FakeError(400, "bad_param", param="mode", allowed=["extents", "view", "window"])
        width = int((q.get("width") or ["1024"])[0])
        headers = {"content-type": "image/png"}
        if mode == "window":
            if "min" not in q or "max" not in q:
                raise FakeError(400, "missing_param", param="min")
            win = [float(v) for v in q["min"][0].split(",")] + [float(v) for v in q["max"][0].split(",")]
        elif mode == "view":
            win = d.view["window"]["min"] + d.view["window"]["max"]
        else:
            ext = self._extents(d.entities.values())
            win = (ext["min"] + ext["max"]) if ext else [0.0, 0.0, 10.0, 10.0]
            if not ext:
                headers["x-librecad-empty"] = "1"
        headers["x-librecad-window"] = ",".join(f"{v:g}" for v in win)
        span = max(win[2] - win[0], 1e-9)
        headers["x-librecad-scale"] = f"{(width - 32) / span:g}"
        return httpx.Response(200, content=PNG_1X1, headers=headers)

    def h_get_view(self, q, body):
        return self.doc(q).view

    def h_set_view(self, q, body):
        allow_only(body, {"action", "min", "max", "factor", "center", "offset", "name"})
        d = self.doc(q)
        action = need(body, "action")
        v = d.view
        if action == "window":
            v["window"] = {"min": need(body, "min"), "max": need(body, "max")}
        elif action == "extents":
            ext = self._extents(d.entities.values()) or {"min": [0.0, 0.0], "max": [10.0, 10.0]}
            v["window"] = ext
        elif action == "named":
            raise FakeError(404, "unknown_view", name=need(body, "name"))
        elif action == "pan" and "offset" in body:
            dx, dy = body["offset"]
            v["window"] = {"min": [v["window"]["min"][0] + dx, v["window"]["min"][1] + dy],
                           "max": [v["window"]["max"][0] + dx, v["window"]["max"][1] + dy]}
        mn, mx = v["window"]["min"], v["window"]["max"]
        v["center"] = [(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2]
        return v

    # --- undo, command line -------------------------------------------------------------------------

    def h_undo(self, q, body):
        allow_only(body, {"steps"})
        d = self.doc(q)
        n = 0
        for _ in range(body.get("steps", 1)):
            if not d.undo_stack:
                break
            d.redo_stack.append((copy.deepcopy(d.entities), set(d.selection)))
            d.entities, d.selection = d.undo_stack.pop()
            n += 1
        d.modified = True
        return {"undone": n, "can_undo": bool(d.undo_stack), "can_redo": bool(d.redo_stack),
                "layers_and_blocks_not_undone": True}

    def h_redo(self, q, body):
        allow_only(body, {"steps"})
        d = self.doc(q)
        n = 0
        for _ in range(body.get("steps", 1)):
            if not d.redo_stack:
                break
            d.undo_stack.append((copy.deepcopy(d.entities), set(d.selection)))
            d.entities, d.selection = d.redo_stack.pop()
            n += 1
        return {"redone": n, "can_undo": bool(d.undo_stack), "can_redo": bool(d.redo_stack)}

    def h_command(self, q, body):
        allow_only(body, {"command", "inputs"})
        if not self.allow_commands:
            raise FakeError(403, "commands_disabled")
        d = self.doc(q)
        self.active = d.id
        cmd = need(body, "command")
        inputs = body.get("inputs", [])
        if not isinstance(cmd, str) or not cmd.strip() or any(not str(i).strip() for i in inputs):
            raise FakeError(400, "bad_param", "empty command or input", param="command")
        if cmd not in COMMANDS:
            return {"executed": [], "output": [f"Unknown command: {cmd}"], "prompt": "Command:",
                    "active_action": None, "unknown_commands": [cmd]}
        output = [f"Command: {cmd}"]
        points: list[list[float]] = []
        for raw in inputs:
            if raw == "escape":
                points = []
                continue
            rel = raw.startswith("@")
            x, y = (float(v) for v in raw.lstrip("@").split(","))
            p = [points[-1][0] + x, points[-1][1] + y] if rel and points else [x, y]
            if cmd == "line" and points:
                d.snapshot()
                self._store(d, {"type": "line", "start": points[-1], "end": p}, d.current_layer)
            points.append(p)
        output.append("Specify first point" if cmd == "line" else "")
        return {"executed": [cmd, *inputs], "output": output, "prompt": "Command:",
                "active_action": None, "unknown_commands": []}

    def h_commands(self, q, body):
        return {"commands": COMMANDS}

    # --- layers and blocks ----------------------------------------------------------------------------

    def _layer_json(self, d: Doc, layer: dict) -> dict:
        count = sum(1 for e in d.entities.values() if e["layer"] == layer["name"])
        return {**layer, "current": layer["name"] == d.current_layer, "entity_count": count}

    def _layer(self, d: Doc, name: str) -> dict:
        if name not in d.layers:
            raise FakeError(404, "unknown_layer", name=name)
        return d.layers[name]

    def h_layers(self, q, body):
        d = self.doc(q)
        return {"layers": [self._layer_json(d, layer) for layer in d.layers.values()], "current": d.current_layer}

    def _layer_style(self, layer: dict, body: dict) -> None:
        if "color" in body:
            c = body["color"]
            if isinstance(c, int) and not isinstance(c, bool) and 1 <= c <= 255:
                layer["color"], layer["aci"] = {1: "#ff0000", 3: "#00ff00", 5: "#0000ff"}.get(c, "#ffffff"), c
            elif isinstance(c, str) and re.fullmatch(r"#[0-9a-fA-F]{6}", c):
                layer["color"], layer["aci"] = c.lower(), None
            else:
                raise FakeError(400, "bad_param", "layer color must be #rrggbb or 1..255", param="color")
        for key in ("linetype", "lineweight", "frozen", "locked", "printable", "construction"):
            if key in body:
                layer[key] = body[key]

    def h_create_layer(self, q, body):
        allow_only(body, {"name", "color", "linetype", "lineweight", "frozen", "locked", "printable",
                          "construction", "make_current"})
        d = self.doc(q)
        name = need(body, "name")
        if name in d.layers:
            raise FakeError(409, "layer_exists", name=name)
        if not name.strip() or LAYER_NAME_BAD.search(name):
            raise FakeError(400, "bad_param", "invalid layer name", param="name")
        layer = Doc._layer(name)
        self._layer_style(layer, body)
        d.layers[name] = layer
        if body.get("make_current"):
            d.current_layer = name
        return 201, {"layer": self._layer_json(d, layer), "undoable": False}

    def h_update_layer(self, q, body, name):
        allow_only(body, {"new_name", "color", "linetype", "lineweight", "frozen", "locked", "printable",
                          "construction"})
        d = self.doc(q)
        layer = self._layer(d, name)
        new = body.get("new_name")
        if new is not None and new != name:
            if name == "0":
                raise FakeError(422, "cannot_rename_layer_0")
            if new in d.layers:
                raise FakeError(409, "layer_exists", name=new)
            d.layers = {(new if k == name else k): v for k, v in d.layers.items()}
            layer["name"] = new
            for e in d.entities.values():
                if e["layer"] == name:
                    e["layer"] = new
            if d.current_layer == name:
                d.current_layer = new
        self._layer_style(layer, body)
        return {"layer": self._layer_json(d, layer), "undoable": False}

    def h_current_layer(self, q, body, name):
        d = self.doc(q)
        layer = self._layer(d, name)
        if layer["frozen"]:
            raise FakeError(409, "layer_frozen", name=name)
        d.current_layer = name
        return {"current": name}

    def h_delete_layer(self, q, body, name):
        d = self.doc(q)
        policy = (q.get("entities") or ["refuse"])[0]
        move_to = (q.get("move_to") or ["0"])[0]
        if policy not in ("refuse", "delete", "move"):
            raise FakeError(400, "bad_param", param="entities", allowed=["refuse", "delete", "move"])
        if name == "0":
            raise FakeError(422, "cannot_delete_layer_0")
        self._layer(d, name)
        ids = [i for i, e in d.entities.items() if e["layer"] == name]
        if ids and policy == "refuse":
            raise FakeError(409, "layer_not_empty", entity_count=len(ids))
        moved = deleted = 0
        if ids and policy == "move":
            self._layer(d, move_to)
            d.snapshot()
            for i in ids:
                d.entities[i]["layer"] = move_to
            moved = len(ids)
        elif ids:
            for i in ids:
                del d.entities[i]
            deleted = len(ids)
        if d.current_layer == name:
            d.current_layer = "0"
        del d.layers[name]
        return {"deleted": name, "entities_deleted": deleted, "entities_moved": moved, "undoable": False}

    def h_blocks(self, q, body):
        d = self.doc(q)
        return {"blocks": [{**b, "insert_count": sum(1 for e in d.entities.values()
                                                     if e["type"] == "insert" and e.get("block") == b["name"])}
                           for b in d.blocks.values()]}

    def h_insert_block(self, q, body, name):
        allow_only(body, {"position", "scale", "angle_deg", "layer", "select", "columns", "rows", "spacing"})
        d = self.doc(q)
        if name not in d.blocks:
            raise FakeError(404, "unknown_block", name=name)
        layer = body.get("layer") or d.current_layer
        self._check_layer_writable(d, layer)
        d.snapshot()
        spec = {"type": "insert", "block": name, "position": need(body, "position"),
                "scale": body.get("scale", 1), "angle_deg": body.get("angle_deg", 0)}
        e = self._store(d, spec, layer)
        return 201, {"entity": e, "undoable": True}

    # --- entities -----------------------------------------------------------------------------------

    def _check_layer_writable(self, d: Doc, layer: str) -> None:
        if layer not in d.layers:
            raise FakeError(404, "unknown_layer", name=layer)
        if d.layers[layer]["locked"]:
            raise FakeError(409, "layer_locked", layer=layer)

    def _store(self, d: Doc, spec: dict, layer: str) -> dict:
        e = {k: v for k, v in spec.items() if k not in ATTRS}
        e.update({"id": self.next_entity, "layer": layer, "color": spec.get("color", "ByLayer"),
                  "linetype": spec.get("linetype", "ByLayer"), "lineweight": spec.get("lineweight", "ByLayer"),
                  "handle": None, "visible": True, "locked": False, "selected": False})
        self._derive(e)
        d.entities[self.next_entity] = e
        self.next_entity += 1
        return e

    @staticmethod
    def _points(e: dict) -> list[list[float]]:
        pts = [e[k] for k in POINT_KEYS if k in e]
        for k in POINT_LIST_KEYS:
            pts.extend(v[:2] for v in e.get(k) or [])
        return pts

    def _derive(self, e: dict) -> None:
        pts = self._points(e)
        r = e.get("radius", 0)
        if pts:
            xs = [p[0] for p in pts]
            ys = [p[1] for p in pts]
            e["bbox"] = {"min": [min(xs) - r, min(ys) - r], "max": [max(xs) + r, max(ys) + r]}
        if e["type"] == "line":
            (x1, y1), (x2, y2) = e["start"], e["end"]
            e["length"] = math.hypot(x2 - x1, y2 - y1)
            e["angle_deg"] = math.degrees(math.atan2(y2 - y1, x2 - x1)) % 360
        if e["type"] == "circle":
            e["length"], e["area"] = 2 * math.pi * r, math.pi * r * r
        if e["type"] == "polyline":
            e["vertex_count"] = len(e["vertices"])

    def _extents(self, entities) -> dict | None:
        boxes = [e["bbox"] for e in entities if "bbox" in e]
        if not boxes:
            return None
        return {"min": [min(b["min"][0] for b in boxes), min(b["min"][1] for b in boxes)],
                "max": [max(b["max"][0] for b in boxes), max(b["max"][1] for b in boxes)]}

    def _live(self, d: Doc, ids) -> list[int]:
        unknown = [i for i in ids if i not in d.entities]
        if unknown:
            raise FakeError(404, "unknown_entity", ids=unknown)
        locked = [i for i in ids if d.layers.get(d.entities[i]["layer"], {}).get("locked")]
        if locked:
            raise FakeError(409, "entity_locked", ids=locked)
        return list(ids)

    def h_entities(self, q, body):
        d = self.doc(q)
        types = [t for v in q.get("type", []) for t in v.split(",")]
        layers = q.get("layer", [])
        out = []
        for e in d.entities.values():
            if types and e["type"] not in types:
                continue
            if layers and e["layer"] not in layers:
                continue
            if q.get("selected", ["false"])[0] == "true" and e["id"] not in d.selection:
                continue
            if "bbox" in q:
                x1, y1, x2, y2 = (float(v) for v in q["bbox"][0].split(","))
                b = e.get("bbox")
                if not b:
                    continue
                mode = q.get("bbox_mode", ["crossing"])[0]
                inside = b["min"][0] >= x1 and b["min"][1] >= y1 and b["max"][0] <= x2 and b["max"][1] <= y2
                touches = not (b["max"][0] < x1 or b["min"][0] > x2 or b["max"][1] < y1 or b["min"][1] > y2)
                if (mode == "window" and not inside) or (mode == "crossing" and not touches):
                    continue
            out.append({**e, "selected": e["id"] in d.selection})
        limit = int(q.get("limit", ["100"])[0])
        offset = int(q.get("offset", ["0"])[0])
        if not 1 <= limit <= 1000:
            raise FakeError(400, "bad_param", param="limit")
        page = out[offset:offset + limit]
        nxt = offset + limit if offset + limit < len(out) else None
        return {"entities": page, "total": len(out), "offset": offset, "limit": limit, "next_offset": nxt}

    def h_entity(self, q, body, id):
        d = self.doc(q)
        i = int(id)
        if i not in d.entities:
            raise FakeError(404, "unknown_entity", ids=[i])
        return {"entity": d.entities[i]}

    def _validate_spec(self, i: int, spec: dict, errors: dict) -> None:
        t = spec.get("type")
        if t not in TYPE_FIELDS:
            errors[f"entities[{i}].type"] = "unknown entity type"
            return
        for k in spec:
            if k != "type" and k not in TYPE_FIELDS[t] and k not in ATTRS:
                errors[f"entities[{i}].{k}"] = f"not a field of {t}"
        if t in ("circle", "arc") and not (isinstance(spec.get("radius"), (int, float)) and spec["radius"] > 0):
            errors[f"entities[{i}].radius"] = "must be > 0"
        if t == "polyline" and len(spec.get("vertices") or []) < 2:
            errors[f"entities[{i}].vertices"] = "needs at least 2 vertices"
        if t in ("text", "mtext") and spec.get("style", "standard") != "standard":
            errors[f"entities[{i}].style"] = "unknown text style"

    def h_create(self, q, body):
        allow_only(body, {"entities", "layer", "select"})
        d = self.doc(q)
        specs = need(body, "entities")
        if len(specs) > 5000:
            raise FakeError(422, "too_many", limit=5000)
        errors: dict = {}
        for i, spec in enumerate(specs):
            self._validate_spec(i, spec, errors)
        if errors:
            raise FakeError(422, "invalid_entity", errors=errors)
        layers = [s.get("layer") or body.get("layer") or d.current_layer for s in specs]
        for layer in layers:
            self._check_layer_writable(d, layer)
        d.snapshot()
        created = [self._store(d, s, layer) for s, layer in zip(specs, layers, strict=True)]
        if body.get("select"):
            d.selection |= {e["id"] for e in created}
        return 201, {"created": created, "ids": [e["id"] for e in created], "undoable": True, "warnings": []}

    def h_update(self, q, body, id):
        d = self.doc(q)
        i = int(id)
        self._live(d, [i])
        e = d.entities[i]
        if "type" in body and body["type"] != e["type"]:
            raise FakeError(400, "bad_param", "the type cannot change", param="type")
        for k in body:
            if k != "type" and k not in TYPE_FIELDS[e["type"]] and k not in ATTRS:
                raise FakeError(400, "unknown_field", f"'{k}' is not a field of {e['type']}", field=k)
        if "layer" in body:
            self._check_layer_writable(d, body["layer"])
        d.snapshot()
        e = d.entities[i]
        e.update({k: v for k, v in body.items() if k != "type"})
        self._derive(e)
        return {"entity": e, "undoable": True}

    def h_delete_one(self, q, body, id):
        return self.h_delete(q, {"ids": [int(id)]})

    def h_delete(self, q, body):
        allow_only(body, {"ids"})
        d = self.doc(q)
        ids = self._live(d, need(body, "ids"))
        d.snapshot()
        for i in ids:
            del d.entities[i]
            d.selection.discard(i)
        return {"deleted": ids, "undoable": True}

    def h_transform(self, q, body):
        allow_only(body, {"ids", "op", "offset", "center", "angle_deg", "base", "factor", "axis", "copy",
                          "copies", "select"})
        d = self.doc(q)
        ids = self._live(d, need(body, "ids"))
        op = need(body, "op")
        if op == "move":
            dx, dy = need(body, "offset")

            def f(p, k=1):
                return [p[0] + dx * k, p[1] + dy * k]
        elif op == "rotate":
            cx, cy = need(body, "center")
            a = math.radians(need(body, "angle_deg"))

            def f(p, k=1):
                x, y = p[0] - cx, p[1] - cy
                c, s = math.cos(a * k), math.sin(a * k)
                return [cx + x * c - y * s, cy + x * s + y * c]
        elif op == "scale":
            bx, by = need(body, "base")
            fac = need(body, "factor")
            fx, fy = (fac, fac) if isinstance(fac, (int, float)) else fac

            def f(p, k=1):
                return [bx + (p[0] - bx) * fx ** k, by + (p[1] - by) * fy ** k]
        elif op == "mirror":
            (x1, y1), (x2, y2) = need(body, "axis")
            ux, uy = x2 - x1, y2 - y1
            n2 = ux * ux + uy * uy

            def f(p, k=1):
                t = ((p[0] - x1) * ux + (p[1] - y1) * uy) / n2
                fx_, fy_ = x1 + t * ux, y1 + t * uy
                return [2 * fx_ - p[0], 2 * fy_ - p[1]]
        else:
            raise FakeError(400, "bad_param", param="op", allowed=["move", "rotate", "scale", "mirror"])

        def moved(e, k):
            e = copy.deepcopy(e)
            for key in POINT_KEYS:
                if key in e:
                    e[key] = f(e[key], k)
            for key in POINT_LIST_KEYS:
                if e.get(key):
                    e[key] = [f(v[:2], k) + v[2:] for v in e[key]]
            self._derive(e)
            return e
        d.snapshot()
        result = []
        if body.get("copy"):
            for k in range(1, body.get("copies", 1) + 1):
                for i in ids:
                    e = moved(d.entities[i], k)
                    e["id"] = self.next_entity
                    d.entities[self.next_entity] = e
                    self.next_entity += 1
                    result.append(e)
        else:
            for i in ids:
                d.entities[i] = moved(d.entities[i], 1)
                result.append(d.entities[i])
        return {"entities": result, "ids": [e["id"] for e in result], "originals_kept": bool(body.get("copy")),
                "undoable": True}

    def h_get_selection(self, q, body):
        d = self.doc(q)
        return {"ids": sorted(d.selection), "count": len(d.selection)}

    def h_set_selection(self, q, body):
        allow_only(body, {"mode", "ids", "bbox", "bbox_mode", "type", "layer"})
        d = self.doc(q)
        mode = need(body, "mode")
        ids = set(body.get("ids") or [])
        unknown = [i for i in ids if i not in d.entities]
        if unknown:
            raise FakeError(404, "unknown_entity", ids=unknown)
        for e in d.entities.values():
            if (body.get("type") and e["type"] in body["type"]) or (body.get("layer") and e["layer"] in body["layer"]):
                ids.add(e["id"])
        if mode == "set":
            d.selection = ids
        elif mode == "add":
            d.selection |= ids
        elif mode == "remove":
            d.selection -= ids
        elif mode == "clear":
            d.selection = set()
        elif mode == "all":
            d.selection = set(d.entities)
        return {"ids": sorted(d.selection), "count": len(d.selection)}

    def h_measure(self, q, body):
        allow_only(body, {"kind", "from", "to", "id", "ids", "points"})
        d = self.doc(q)
        kind = need(body, "kind")
        if kind == "distance":
            (x1, y1) = need(body, "from")
            if "to" in body:
                x2, y2 = body["to"]
                return {"distance": math.hypot(x2 - x1, y2 - y1), "dx": x2 - x1, "dy": y2 - y1,
                        "angle_deg": math.degrees(math.atan2(y2 - y1, x2 - x1))}
            self._live(d, [need(body, "id")])
            return {"distance": 0.0, "nearest_point": [x1, y1]}
        if kind == "length":
            ids = self._live(d, need(body, "ids"))
            per = {str(i): d.entities[i].get("length", 0.0) for i in ids}
            return {"length": sum(per.values()), "per_entity": per}
        if kind == "area":
            pts = body.get("points")
            if pts is None:
                ids = self._live(d, need(body, "ids"))
                open_ids = [i for i in ids if not (d.entities[i]["type"] == "circle"
                                                   or d.entities[i].get("closed"))]
                if open_ids:
                    raise FakeError(422, "not_closed", ids=open_ids)
                e = d.entities[ids[0]]
                if e["type"] == "circle":
                    return {"area": e["area"], "perimeter": e["length"], "per_entity": {str(ids[0]): e["area"]}}
                pts = [v[:2] for v in e["vertices"]]
            area = 0.5 * abs(sum(pts[i][0] * pts[(i + 1) % len(pts)][1] - pts[(i + 1) % len(pts)][0] * pts[i][1]
                                 for i in range(len(pts))))
            per = sum(math.dist(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts)))
            return {"area": area, "perimeter": per, "per_entity": {}}
        if kind == "angle":
            pts = need(body, "points")
            (vx, vy), (ax, ay), (bx, by) = pts
            a = math.degrees(math.atan2(by - vy, bx - vx) - math.atan2(ay - vy, ax - vx)) % 360
            return {"angle_deg": a}
        if kind == "intersections":
            a, b = (d.entities[i] for i in self._live(d, need(body, "ids")))
            (x1, y1), (x2, y2), (x3, y3), (x4, y4) = a["start"], a["end"], b["start"], b["end"]
            den = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4)
            if den == 0:
                return {"points": []}
            t = ((x1 - x3) * (y3 - y4) - (y1 - y3) * (x3 - x4)) / den
            return {"points": [[x1 + t * (x2 - x1), y1 + t * (y2 - y1)]]}
        raise FakeError(400, "bad_param", param="kind")


def body_of(call: dict) -> Any:
    return call["body"]
