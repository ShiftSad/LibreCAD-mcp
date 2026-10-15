# SPDX-License-Identifier: GPL-2.0-or-later
"""MCP server for LibreCAD: drives a running LibreCAD (the LibreCAD-mcp build) through its local
Remote API."""
from __future__ import annotations

try:
    from importlib.metadata import version as _dist_version

    __version__ = _dist_version("librecad-mcp")
except Exception:  # source checkout without an installed distribution
    __version__ = "0.0.0"

__all__ = ["__version__"]
