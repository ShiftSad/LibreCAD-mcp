# SPDX-License-Identifier: GPL-2.0-or-later
"""A short scenario against a running LibreCAD (the LibreCAD-mcp build) with its Remote API on.

Skipped unless LIBRECAD_LIVE=1. The connection comes from the usual settings (discovery file, or
LIBRECAD_API_URL/LIBRECAD_API_TOKEN/LIBRECAD_API_DISCOVERY). It opens a new drawing, draws in it,
and closes it again without saving; the user's other drawings are not touched.
"""
import os

import pytest

pytestmark = pytest.mark.skipif(os.environ.get("LIBRECAD_LIVE") != "1", reason="set LIBRECAD_LIVE=1 to run")

_LIVE_ENV = {k: v for k, v in os.environ.items() if k.startswith("LIBRECAD_")}


@pytest.fixture(autouse=True)
def _restore_live_settings(monkeypatch):
    # conftest clears LIBRECAD_* for the unit tests; put the live connection settings back.
    for k, v in _LIVE_ENV.items():
        monkeypatch.setenv(k, v)


async def test_live_scenario():
    import librecad_mcp.server as srv
    from helpers import call

    status = await srv.get_status()
    assert status.get("app") == "LibreCAD", status
    doc_id = (await srv.new_document())["document"]["id"]
    try:
        created = await call("add_entities", {"document_id": doc_id, "entities": [
            {"type": "polyline", "vertices": [[0, 0], [100, 0], [100, 50], [0, 50]], "closed": True},
            {"type": "circle", "center": [50, 25], "radius": 10},
            {"type": "text", "position": [5, 55], "text": "live test", "height": 5},
        ]})
        assert len(created["ids"]) == 3, created
        area = await srv.measure("area", entity_ids=[created["ids"][0]], document_id=doc_id)
        assert abs(area["area"] - 5000) < 1e-6, area
        image = await srv.render_view(document_id=doc_id, width_px=320, height_px=200)
        assert isinstance(image, list) and image[0].data[:8] == b"\x89PNG\r\n\x1a\n", image
        found = await srv.query_entities(types=["circle"], document_id=doc_id)
        assert found["total"] == 1, found
        assert (await srv.undo(document_id=doc_id))["undone"] == 1
        assert (await srv.get_drawing_info(doc_id))["counts"]["total"] == 0
    finally:
        await srv.close_document(doc_id, discard_changes=True, user_confirmed=True)
