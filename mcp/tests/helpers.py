# SPDX-License-Identifier: GPL-2.0-or-later
"""Calling tools the way an MCP client does: through the SDK's in-memory Client."""
from __future__ import annotations

import json

import mcp.types as types
from mcp.client.client import Client

import librecad_mcp.server as srv

MODES = ["legacy", "auto"]  # a request in the middle of the call (<= 2025-11-25), input-required rounds


class Asker:
    """An MCP client's elicitation handler: records each question and gives a fixed answer."""

    def __init__(self, action: str = "accept", confirm: bool = True):
        self.action, self.confirm, self.questions = action, confirm, []

    async def __call__(self, context, params):
        self.questions.append(params.message)
        if self.action == "accept":
            return types.ElicitResult(action="accept", content={"confirm": self.confirm})
        return types.ElicitResult(action=self.action)


async def call_raw(name: str, arguments: dict | None = None, mode: str = "legacy", asker: Asker | None = None):
    kwargs = {"mode": mode}
    if asker is not None:
        kwargs["elicitation_callback"] = asker
    async with Client(srv.mcp, **kwargs) as client:
        return await client.call_tool(name, arguments or {})


async def call(name: str, arguments: dict | None = None, mode: str = "legacy", asker: Asker | None = None) -> dict:
    """The tool's JSON reply as a dict (asserts it was a normal, non-error result)."""
    result = await call_raw(name, arguments, mode, asker)
    assert not result.is_error, result.content[0].text
    assert len(result.content) == 1 and result.content[0].type == "text", result.content
    return json.loads(result.content[0].text)


def requests(fake, method: str | None = None, path: str | None = None) -> list[dict]:
    return [c for c in fake.calls
            if (method is None or c["method"] == method) and (path is None or c["path"] == "/api/v1" + path)]
