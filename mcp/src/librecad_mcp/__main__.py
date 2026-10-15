# SPDX-License-Identifier: GPL-2.0-or-later
"""``python -m librecad_mcp``: run the stdio MCP server, as the ``librecad-mcp`` command does."""
from .server import main

if __name__ == "__main__":
    main()
