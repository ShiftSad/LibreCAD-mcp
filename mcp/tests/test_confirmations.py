# SPDX-License-Identifier: GPL-2.0-or-later
"""Destructive actions ask the user: MCP elicitation when the client can show a prompt, else
user_confirmed. A prompt the user saw always beats user_confirmed (DESIGN.md section 4.6)."""
import pytest

import librecad_mcp.server as srv
from helpers import MODES, Asker, call, requests


# --- close_document(discard_changes=True) ---------------------------------------------------------

async def test_discard_without_confirmation_is_refused(fake, doc):
    doc.modified = True
    out = await srv.close_document(discard_changes=True)
    assert out["error"] == "confirmation_required"
    assert out["message"] == f"Close '{doc.name}' in LibreCAD and discard its unsaved changes? They cannot be recovered."
    assert doc.id in fake.docs
    assert all(c["body"] != {"discard_changes": True} for c in requests(fake, "POST", "/documents/close"))


async def test_discard_with_user_confirmed_when_the_client_cannot_ask(fake, doc):
    doc.modified = True
    out = await srv.close_document(discard_changes=True, user_confirmed=True)
    assert out == {"closed": True, "discarded_changes": True}


async def test_discard_on_an_unmodified_drawing_needs_no_question(fake, doc):
    asker = Asker()
    out = await call("close_document", {"discard_changes": True}, asker=asker)
    assert out == {"closed": True, "discarded_changes": False} and asker.questions == []


@pytest.mark.parametrize("mode", MODES)
async def test_discard_asks_the_user_and_a_yes_closes(fake, doc, mode):
    doc.modified = True
    asker = Asker()
    out = await call("close_document", {"discard_changes": True}, mode, asker)
    assert asker.questions == [f"Close '{doc.name}' in LibreCAD and discard its unsaved changes? They cannot be recovered."]
    assert out == {"closed": True, "discarded_changes": True}


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("action,confirm,how", [("decline", True, "said no"),
                                                ("cancel", True, "closed the confirmation"),
                                                ("accept", False, "did not tick the confirmation")])
async def test_anything_but_a_ticked_yes_keeps_the_drawing(fake, doc, mode, action, confirm, how):
    doc.modified = True
    out = await call("close_document", {"discard_changes": True, "user_confirmed": True}, mode, Asker(action, confirm))
    assert out["error"] == "not_confirmed" and how in out["message"]
    assert "the drawing was not closed" in out["message"]
    assert doc.id in fake.docs


# --- delete_layer -------------------------------------------------------------------------------------

async def _layer_with_entities(fake, name="walls", count=2):
    await srv.create_layer(name)
    if count:
        await call("add_entities", {"entities": [{"type": "point", "position": [i, 0]} for i in range(count)],
                                    "layer": name})


async def test_delete_layer_without_confirmation(fake, doc):
    await _layer_with_entities(fake, count=0)
    out = await srv.delete_layer("walls")
    assert out["error"] == "confirmation_required"
    assert out["message"] == "Delete layer 'walls' (it must be empty)? Deleting a layer cannot be undone in LibreCAD."
    assert "walls" in doc.layers and requests(fake, "DELETE") == []
    out = await srv.delete_layer("walls", user_confirmed=True)
    assert out == {"deleted": "walls", "entities_deleted": 0, "entities_moved": 0, "undoable": False}


@pytest.mark.parametrize("mode", MODES)
async def test_a_declined_prompt_beats_user_confirmed(fake, doc, mode):
    await _layer_with_entities(fake)
    asker = Asker("decline")
    out = await call("delete_layer", {"name": "walls", "entities": "delete", "user_confirmed": True}, mode, asker)
    assert asker.questions == ["Delete layer 'walls' and delete 2 entities on it? Deleting a layer cannot be "
                               "undone in LibreCAD."]
    assert out["error"] == "not_confirmed" and "said no" in out["message"]
    assert "walls" in doc.layers and len(doc.entities) == 2 and requests(fake, "DELETE") == []


@pytest.mark.parametrize("mode", MODES)
async def test_a_yes_deletes_and_moves(fake, doc, mode):
    await _layer_with_entities(fake)
    asker = Asker()
    out = await call("delete_layer", {"name": "walls", "entities": "move", "move_to": "0"}, mode, asker)
    assert "move 2 entities to layer '0'" in asker.questions[0]
    assert out["entities_moved"] == 2 and "walls" not in doc.layers
    assert {e["layer"] for e in doc.entities.values()} == {"0"}
    assert requests(fake, "DELETE")[-1]["query"] == {"entities": ["move"], "move_to": ["0"]}


async def test_refusing_a_non_empty_layer_asks_nothing(fake, doc):
    await _layer_with_entities(fake)
    asker = Asker()
    out = await call("delete_layer", {"name": "walls"}, asker=asker)
    assert out["error"] == "layer_not_empty" and out["entity_count"] == 2 and "entities='move'" in out["hint"]
    assert asker.questions == []
    assert (await srv.delete_layer("walls"))["error"] == "layer_not_empty"  # no confirmation_required either


async def test_unknown_layer_and_layer_0_ask_nothing(fake, doc):
    asker = Asker()
    assert (await call("delete_layer", {"name": "ghost"}, asker=asker))["error"] == "unknown_layer"
    assert (await call("delete_layer", {"name": "0", "entities": "delete"}, asker=asker))["error"] == "cannot_delete_layer_0"
    assert asker.questions == []


async def test_a_locked_layer_with_entities_is_refused_without_asking(fake, doc):
    await _layer_with_entities(fake)
    await srv.update_layer("walls", locked=True)
    asker = Asker()
    out = await call("delete_layer", {"name": "walls", "entities": "delete"}, asker=asker)
    assert out["error"] == "layer_locked" and out["layer"] == "walls" and "update_layer" in out["hint"]
    assert asker.questions == [] and "walls" in doc.layers and len(doc.entities) == 2
    # a locked target of the move is refused by LibreCAD too (after the usual question)
    await srv.update_layer("walls", locked=False)
    await srv.create_layer("archive", locked=True)
    out = await srv.delete_layer("walls", entities="move", move_to="archive", user_confirmed=True)
    assert out["error"] == "layer_locked" and out["layer"] == "archive" and "walls" in doc.layers


# --- overwriting files ------------------------------------------------------------------------------------

async def test_overwrite_of_an_existing_file_needs_confirmation(fake, doc, tmp_path):
    target = tmp_path / "plan.dxf"
    target.write_bytes(b"precious")
    out = await srv.save_document_as(str(target))
    assert out["error"] == "file_exists" and out["path"] == str(target) and "overwrite=True" in out["hint"]
    out = await srv.save_document_as(str(target), overwrite=True)
    assert out["error"] == "confirmation_required" and str(target) in out["message"]
    assert target.read_bytes() == b"precious"
    out = await srv.save_document_as(str(target), overwrite=True, user_confirmed=True)
    assert out["path"] == str(target) and target.read_bytes() != b"precious"
    assert requests(fake, "POST", "/documents/save_as")[-1]["body"]["overwrite"] is True


async def test_overwrite_of_a_missing_file_needs_no_question_and_sends_no_overwrite(fake, doc, tmp_path):
    asker = Asker()
    out = await call("export_document", {"path": str(tmp_path / "new.png"), "format": "png", "overwrite": True},
                     asker=asker)
    assert out["format"] == "png" and asker.questions == []
    assert requests(fake, "POST", "/documents/export")[-1]["body"]["overwrite"] is False


@pytest.mark.parametrize("mode", MODES)
async def test_overwrite_prompt(fake, doc, tmp_path, mode):
    target = tmp_path / "out.svg"
    target.write_bytes(b"old")
    asker = Asker("decline")
    out = await call("export_document", {"path": str(target), "format": "svg", "overwrite": True,
                                         "user_confirmed": True}, mode, asker)
    assert out["error"] == "not_confirmed" and target.read_bytes() == b"old"
    assert asker.questions == [f"Replace the existing file {target}? Its current content will be lost."]
    out = await call("export_document", {"path": str(target), "format": "svg", "overwrite": True}, mode, Asker())
    assert out["format"] == "svg" and target.read_bytes() != b"old"


@pytest.mark.parametrize("mode", MODES)
async def test_the_overwrite_question_sees_default_arguments(fake, doc, tmp_path, mode):
    target = tmp_path / "plan.dxf"
    target.write_bytes(b"old")
    asker = Asker()
    out = await call("save_document_as", {"path": str(target), "overwrite": True}, mode, asker)  # format omitted
    assert asker.questions == [f"Replace the existing file {target}? Its current content will be lost."]
    assert out["path"] == str(target) and target.read_bytes() != b"old"


async def test_a_refused_path_is_never_asked_about(fake, doc):
    asker = Asker()
    out = await call("save_document_as", {"path": "relative.dxf", "overwrite": True}, asker=asker)
    assert out["error"] == "blocked_by_local_policy" and asker.questions == []


# --- a layer name in another Unicode spelling is the same layer ------------------------------------

NFC = "Caf" + chr(0xE9)     # how the layer was created
NFD = "Cafe" + chr(0x301)    # the same name, decomposed: LibreCAD finds the same layer


async def test_delete_layer_asks_for_any_spelling_of_the_name(fake, doc):
    await _layer_with_entities(fake, name=NFC)
    out = await srv.delete_layer(NFD, entities="delete")
    assert out["error"] == "confirmation_required"
    assert NFC in doc.layers and requests(fake, "DELETE") == []


@pytest.mark.parametrize("mode", MODES)
async def test_delete_layer_prompts_for_any_spelling_and_deletes_the_listed_layer(fake, doc, mode):
    await _layer_with_entities(fake, name=NFC)
    asker = Asker()
    out = await call("delete_layer", {"name": NFD, "entities": "delete"}, mode, asker)
    assert asker.questions == [f"Delete layer '{NFC}' and delete 2 entities on it? Deleting a layer cannot be "
                               "undone in LibreCAD."]
    assert out["deleted"] == NFC
    from urllib.parse import quote
    assert [c["path"] for c in requests(fake, "DELETE")] == ["/api/v1/layers/" + quote(NFC, safe="")]


async def test_delete_layer_of_an_unknown_name_deletes_nothing(fake, doc):
    out = await srv.delete_layer("nope", user_confirmed=True)
    assert out["error"] == "unknown_layer" and out["name"] == "nope"
    assert requests(fake, "DELETE") == []
