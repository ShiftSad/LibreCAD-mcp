# SPDX-License-Identifier: GPL-2.0-or-later
"""Async HTTP client for LibreCAD's Remote API (``/api/v1``, DESIGN.md section 2).

One method per route. Every method raises an ``errors.ApiError`` subclass on failure and returns
the decoded JSON object (``render`` returns the PNG bytes plus the window it shows).
"""
from __future__ import annotations

from urllib.parse import quote

import httpx

from . import __version__
from .config import LONG_TIMEOUT, Config
from .errors import ApiError, NotReachable, ServerError, UiTimeout, Unauthorized, error_from_status

API = "/api/v1"


def _clean(d: dict | None) -> dict:
    return {k: v for k, v in (d or {}).items() if v is not None}


def _seg(name: str) -> str:
    """A path segment (layer or block name), percent-encoded including '/'."""
    return quote(name, safe="")


def _floats(text: str | None, count: int) -> list[float] | None:
    if not text:
        return None
    try:
        values = [float(v) for v in text.split(",")]
    except ValueError:
        return None
    return values if len(values) == count else None


class LibreCadClient:
    def __init__(self, cfg: Config, transport: httpx.AsyncBaseTransport | None = None):
        self._cfg = cfg
        self._http = httpx.AsyncClient(
            base_url=cfg.base_url,
            headers={"X-Api-Token": cfg.token, "Accept": "application/json",
                     "User-Agent": f"librecad-mcp/{__version__}"},
            timeout=cfg.timeout,
            transport=transport,
            # Never route the token through a proxy from HTTP_PROXY & co: the API is local.
            trust_env=False,
            follow_redirects=False,
        )

    @property
    def config(self) -> Config:
        return self._cfg

    async def __aenter__(self) -> "LibreCadClient":
        return self

    async def __aexit__(self, *exc) -> None:
        await self._http.aclose()

    # --- transport ---------------------------------------------------------------------------

    async def _send(self, method: str, path: str, *, json: dict | None = None,
                    params: dict | None = None, doc: int | None = None,
                    timeout: float | None = None) -> httpx.Response:
        query = _clean(params)
        if doc is not None:
            query["doc"] = doc
        wait = timeout if timeout is not None else self._cfg.timeout
        # Tell LibreCAD when this call stops waiting, a little before it does: LibreCAD then answers
        # 504 ui_timeout (with "started") itself, and never runs a request whose caller is gone.
        headers = {"X-Request-Timeout-Ms": str(max(1, int((wait - min(2.0, wait * 0.1)) * 1000)))}
        try:
            resp = await self._http.request(method, API + path, json=json, params=query or None,
                                            timeout=wait, headers=headers)
        except httpx.TimeoutException as e:
            raise UiTimeout(f"LibreCAD did not answer within {wait:g} s", client_side=True) from e
        except httpx.TransportError as e:
            raise NotReachable(
                f"LibreCAD not reachable at {self._cfg.base_url}: is it running with Preferences > "
                f"Remote API (MCP) enabled? ({type(e).__name__})") from e
        if resp.status_code >= 400:
            try:
                body = resp.json()
            except Exception:  # noqa: BLE001 - a non-JSON error body still maps by status
                body = {}
            err = error_from_status(resp.status_code, body)
            if isinstance(err, Unauthorized) and err.body.get("app") != "LibreCAD":
                err.body = {**err.body, "hint": (
                    f"The program answering at {self._cfg.base_url} did not identify itself as "
                    "LibreCAD: check the port in LibreCAD's Preferences > Remote API (MCP) and "
                    "LIBRECAD_API_URL.")}
            raise err
        return resp

    async def _request(self, method: str, path: str, **kw) -> dict:
        resp = await self._send(method, path, **kw)
        try:
            data = resp.json()
        except Exception as e:  # noqa: BLE001
            raise ServerError("invalid_response") from e
        if not isinstance(data, dict):
            raise ServerError("invalid_response")
        return data

    async def has_capability(self, capability: str) -> bool:
        caps = (await self.status()).get("capabilities") or []
        return capability in caps

    # --- meta --------------------------------------------------------------------------------

    async def status(self) -> dict:
        return await self._request("GET", "/status")

    async def routes(self) -> dict:
        return await self._request("GET", "/routes")

    async def formats(self) -> dict:
        return await self._request("GET", "/formats")

    # --- documents ---------------------------------------------------------------------------

    async def documents(self) -> dict:
        return await self._request("GET", "/documents")

    async def new_document(self, activate: bool = True) -> dict:
        return await self._request("POST", "/documents/new", json={"activate": activate},
                                   timeout=LONG_TIMEOUT)

    async def open_document(self, path: str, accept_partial: bool = False) -> dict:
        return await self._request("POST", "/documents/open",
                                   json={"path": path, "accept_partial": accept_partial},
                                   timeout=LONG_TIMEOUT)

    async def activate_document(self, doc: int) -> dict:
        return await self._request("POST", "/documents/activate", json={}, doc=doc)

    async def save_document(self, doc: int | None = None) -> dict:
        return await self._request("POST", "/documents/save", json={}, doc=doc, timeout=LONG_TIMEOUT)

    async def save_document_as(self, doc: int | None, path: str, fmt: str = "dxf",
                               version: str | None = None, overwrite: bool = False) -> dict:
        body = _clean({"path": path, "format": fmt, "version": version, "overwrite": overwrite})
        return await self._request("POST", "/documents/save_as", json=body, doc=doc,
                                   timeout=LONG_TIMEOUT)

    async def export_document(self, doc: int | None, body: dict) -> dict:
        return await self._request("POST", "/documents/export", json=_clean(body), doc=doc,
                                   timeout=LONG_TIMEOUT)

    async def close_document(self, doc: int | None = None, discard_changes: bool = False) -> dict:
        return await self._request("POST", "/documents/close",
                                   json={"discard_changes": discard_changes}, doc=doc)

    async def drawing(self, doc: int | None = None) -> dict:
        return await self._request("GET", "/drawing", doc=doc)

    # --- view and render ---------------------------------------------------------------------

    async def render(self, doc: int | None = None, **params) -> tuple[bytes, dict]:
        """PNG bytes and ``{"window": {"min", "max"} | None, "px_per_unit", "empty"}``."""
        # LibreCAD's render route may take 30 s; wait a little longer so its own 504 arrives.
        resp = await self._send("GET", "/render", params=params, doc=doc, timeout=LONG_TIMEOUT)
        ctype = resp.headers.get("content-type", "")
        if not ctype.startswith("image/png"):
            raise ServerError("invalid_response")
        window = _floats(resp.headers.get("x-librecad-window"), 4)
        scale = _floats(resp.headers.get("x-librecad-scale"), 1)
        meta = {
            "window": {"min": window[:2], "max": window[2:]} if window else None,
            "px_per_unit": scale[0] if scale else None,
            "empty": resp.headers.get("x-librecad-empty") == "1",
        }
        return resp.content, meta

    async def get_view(self, doc: int | None = None) -> dict:
        return await self._request("GET", "/view", doc=doc)

    async def set_view(self, doc: int | None, body: dict) -> dict:
        return await self._request("POST", "/view", json=_clean(body), doc=doc)

    # --- undo, command line --------------------------------------------------------------------

    async def undo(self, doc: int | None = None, steps: int = 1) -> dict:
        return await self._request("POST", "/undo", json={"steps": steps}, doc=doc)

    async def redo(self, doc: int | None = None, steps: int = 1) -> dict:
        return await self._request("POST", "/redo", json={"steps": steps}, doc=doc)

    async def command(self, doc: int | None, command: str, inputs: list[str] | None = None) -> dict:
        return await self._request("POST", "/command", json={"command": command, "inputs": inputs or []},
                                   doc=doc, timeout=LONG_TIMEOUT)

    async def commands(self) -> dict:
        return await self._request("GET", "/commands")

    # --- layers and blocks ---------------------------------------------------------------------

    async def layers(self, doc: int | None = None) -> dict:
        return await self._request("GET", "/layers", doc=doc)

    async def create_layer(self, doc: int | None, body: dict) -> dict:
        return await self._request("POST", "/layers", json=_clean(body), doc=doc)

    async def update_layer(self, doc: int | None, name: str, body: dict) -> dict:
        return await self._request("PATCH", f"/layers/{_seg(name)}", json=_clean(body), doc=doc)

    async def set_current_layer(self, doc: int | None, name: str) -> dict:
        return await self._request("POST", f"/layers/{_seg(name)}/current", json={}, doc=doc)

    async def delete_layer(self, doc: int | None, name: str, entities: str = "refuse",
                           move_to: str | None = None) -> dict:
        params = {"entities": entities, "move_to": move_to if entities == "move" else None}
        return await self._request("DELETE", f"/layers/{_seg(name)}", params=params, doc=doc)

    async def blocks(self, doc: int | None = None) -> dict:
        return await self._request("GET", "/blocks", doc=doc)

    async def insert_block(self, doc: int | None, name: str, body: dict) -> dict:
        return await self._request("POST", f"/blocks/{_seg(name)}/insert", json=_clean(body), doc=doc)

    # --- entities ------------------------------------------------------------------------------

    async def entities(self, doc: int | None = None, params: dict | None = None) -> dict:
        return await self._request("GET", "/entities", params=params, doc=doc)

    async def entity(self, doc: int | None, entity_id: int) -> dict:
        return await self._request("GET", f"/entities/{int(entity_id)}", doc=doc)

    async def create_entities(self, doc: int | None, body: dict) -> dict:
        return await self._request("POST", "/entities", json=_clean(body), doc=doc)

    async def update_entity(self, doc: int | None, entity_id: int, body: dict) -> dict:
        return await self._request("PATCH", f"/entities/{int(entity_id)}", json=body, doc=doc)

    async def delete_entity(self, doc: int | None, entity_id: int) -> dict:
        return await self._request("DELETE", f"/entities/{int(entity_id)}", doc=doc)

    async def delete_entities(self, doc: int | None, ids: list[int]) -> dict:
        return await self._request("POST", "/entities/delete", json={"ids": list(ids)}, doc=doc)

    async def transform(self, doc: int | None, body: dict) -> dict:
        return await self._request("POST", "/entities/transform", json=_clean(body), doc=doc)

    async def selection(self, doc: int | None = None) -> dict:
        return await self._request("GET", "/selection", doc=doc)

    async def select(self, doc: int | None, body: dict) -> dict:
        return await self._request("POST", "/selection", json=_clean(body), doc=doc)

    async def measure(self, doc: int | None, body: dict) -> dict:
        return await self._request("POST", "/measure", json=_clean(body), doc=doc)


__all__ = ["LibreCadClient", "ApiError", "API"]
