# SPDX-License-Identifier: GPL-2.0-or-later
"""Exceptions for LibreCAD Remote API replies and their mapping from HTTP statuses.

The app answers every failure with a JSON envelope ``{"error": "<token>", "detail"?, "hint"?,
...context}`` (DESIGN.md section 2.2). ``error_from_status`` turns a status plus that body into
one of the classes below and keeps the parsed body as ``err.body``, so the tools can pass the
context fields (``ids``, ``path``, ``allowed``, ``errors``, ...) on to the model.
"""
from __future__ import annotations


class ApiError(Exception):
    """Base class. ``body`` is the app's JSON error body (empty for errors raised here)."""

    body: dict = {}

    @property
    def token(self) -> str:
        """The app's error token, or this error's message when the app sent none."""
        tok = self.body.get("error") if isinstance(self.body, dict) else None
        return tok if isinstance(tok, str) and tok else str(self)


class Unauthorized(ApiError):
    pass


class Forbidden(ApiError):
    pass


class NotFound(ApiError):
    """404. ``route_missing=True`` means the route itself does not exist (a bare
    ``{"error": "not_found"}``, so an older LibreCAD-mcp build); False means the route exists
    but the resource does not (``unknown_entity``, ``file_not_found``, ...)."""

    def __init__(self, message: str, route_missing: bool = False):
        super().__init__(message)
        self.route_missing = route_missing


class BadRequest(ApiError):
    pass


class Conflict(ApiError):
    pass


class Validation(ApiError):
    """422. ``errors`` maps a field path (``entities[2].radius``) to the reason."""

    def __init__(self, message: str, errors: dict | None = None):
        super().__init__(message)
        self.errors = errors if isinstance(errors, dict) else {}


class UiTimeout(ApiError):
    """LibreCAD did not answer in time. ``client_side`` is True when this MCP server gave up
    waiting (its own timeout), False when LibreCAD answered 504 ``ui_timeout``."""

    def __init__(self, message: str, client_side: bool = False):
        super().__init__(message)
        self.client_side = client_side


class NotReachable(ApiError):
    pass


class ConfigError(ApiError):
    pass


class ServerError(ApiError):
    pass


UNAUTHORIZED_MESSAGE = ("unauthorized (check the token: Preferences > Remote API (MCP) in LibreCAD, "
                        "or LIBRECAD_API_TOKEN)")


def error_from_status(status: int, body: object) -> ApiError:
    """The exception for an HTTP error reply; the parsed JSON body is attached as ``.body``."""
    body = body if isinstance(body, dict) else {}
    err = _error_for_status(status, body)
    err.body = body
    return err


def _error_for_status(status: int, body: dict) -> ApiError:
    token = body.get("error")
    token = token if isinstance(token, str) and token else None
    msg = token or (body.get("detail") if isinstance(body.get("detail"), str) else None) or f"HTTP {status}"
    if status == 401:
        return Unauthorized(UNAUTHORIZED_MESSAGE)
    if status == 403:
        return Forbidden(msg)
    if status == 404:
        # A bare not_found (or no token at all) is the router's answer for an unknown route;
        # every missing resource has its own token (unknown_entity, file_not_found, ...).
        if token and token != "not_found":
            return NotFound(token)
        return NotFound("not_found", route_missing=True)
    if status == 400:
        return BadRequest(msg)
    if status == 409:
        return Conflict(msg)
    if status == 422:
        return Validation(msg, body.get("errors"))
    if status == 504:
        return UiTimeout(token or "ui_timeout")
    return ServerError(msg)
