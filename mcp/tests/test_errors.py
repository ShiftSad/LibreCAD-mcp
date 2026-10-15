# SPDX-License-Identifier: GPL-2.0-or-later
import pytest

from librecad_mcp.errors import (BadRequest, Conflict, Forbidden, NotFound, ServerError, UiTimeout,
                                 Unauthorized, Validation, error_from_status)


@pytest.mark.parametrize("status,body,cls,text", [
    (401, {"error": "unauthorized", "app": "LibreCAD"}, Unauthorized, "unauthorized (check the token"),
    (403, {"error": "path_not_allowed", "path": "C:/x"}, Forbidden, "path_not_allowed"),
    (403, {"error": "commands_disabled"}, Forbidden, "commands_disabled"),
    (400, {"error": "unknown_field", "field": "bogus"}, BadRequest, "unknown_field"),
    (409, {"error": "file_exists", "path": "C:/a.dxf"}, Conflict, "file_exists"),
    (422, {"error": "invalid_entity", "errors": {"entities[0].radius": "must be > 0"}}, Validation, "invalid_entity"),
    (504, {"error": "ui_timeout", "started": False}, UiTimeout, "ui_timeout"),
    (500, {"error": "internal", "detail": "boom"}, ServerError, "internal"),
    (501, {"error": "dwg_unsupported"}, ServerError, "dwg_unsupported"),
    (503, {"error": "shutting_down"}, ServerError, "shutting_down"),
    (502, {}, ServerError, "HTTP 502"),
])
def test_status_table(status, body, cls, text):
    err = error_from_status(status, body)
    assert type(err) is cls
    assert str(err).startswith(text)
    assert err.body == body


def test_bare_404_means_the_route_is_missing():
    err = error_from_status(404, {"error": "not_found"})
    assert isinstance(err, NotFound) and err.route_missing
    err = error_from_status(404, {})
    assert isinstance(err, NotFound) and err.route_missing


@pytest.mark.parametrize("token", ["unknown_entity", "unknown_layer", "unknown_document", "file_not_found",
                                   "unknown_block", "unknown_view"])
def test_specific_404_is_a_missing_resource(token):
    err = error_from_status(404, {"error": token, "ids": [3]})
    assert isinstance(err, NotFound) and not err.route_missing
    assert str(err) == token and err.body["ids"] == [3]


def test_validation_keeps_the_field_errors():
    err = error_from_status(422, {"error": "invalid_entity", "errors": {"entities[2].radius": "must be > 0"}})
    assert err.errors == {"entities[2].radius": "must be > 0"}


def test_server_ui_timeout_is_not_client_side():
    assert not error_from_status(504, {"error": "ui_timeout"}).client_side


def test_non_dict_body_is_tolerated():
    err = error_from_status(409, ["x"])
    assert isinstance(err, Conflict) and err.body == {} and str(err) == "HTTP 409"


def test_token_property():
    assert error_from_status(401, {"error": "unauthorized"}).token == "unauthorized"
    assert error_from_status(409, {"error": "unsaved_changes"}).token == "unsaved_changes"
