# Changelog

All notable changes to the LibreCAD MCP server are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - unreleased

First version, for LibreCAD-mcp `v2.2.2.6-mcp.1` (Remote API v1).

### Added
- A stdio MCP server (`librecad-mcp`, MCP Python SDK v2) that drives a running LibreCAD-mcp through
  its local Remote API. 33 tools:
  - session: `get_status`, `list_formats`, `list_documents`;
  - drawings: `new_document`, `open_document`, `activate_document`, `save_document`,
    `save_document_as`, `export_document` (DXF R12..2018, DWG 2000..2018 = AutoCAD 2018-2026,
    PDF, PNG, SVG), `close_document`;
  - looking: `get_drawing_info`, `render_view` (returns the PNG as an image plus its window and
    scale), `get_view`, `set_view`;
  - layers and blocks: `list_layers`, `create_layer`, `update_layer`, `set_current_layer`,
    `delete_layer`, `list_blocks`, `insert_block`;
  - entities: `query_entities`, `get_entity`, `add_entities` (12 entity types, validated before
    anything is sent, atomic, one undo step), `update_entity`, `delete_entities`,
    `transform_entities`, `select_entities`, `measure`;
  - history and commands: `undo`, `redo`, `run_command`, `list_commands`.
- Resources `librecad://guide` (offline reference) and `librecad://routes` (live route list);
  prompts `draw-from-description`, `inspect-drawing` and `convert-file`.
- Zero configuration on the same computer: the address and token come from LibreCAD's discovery
  file; `LIBRECAD_API_URL`, `LIBRECAD_API_TOKEN` and `LIBRECAD_API_DISCOVERY` override it.
- Confirmation by the user (MCP elicitation, else `user_confirmed`) before closing with unsaved
  changes, deleting a layer or overwriting a file; a declined prompt always wins.
- Client-side path rules (absolute local paths, no network paths unless allowed, optional folder
  allow-list, extension per format), checked again inside LibreCAD.
- Tools never fail the call: errors come back as `{"error", "detail", "hint", ...}`, a missing
  route says which capability the LibreCAD build lacks, and a call that runs into the 50 s cap
  answers `timed_out` with a hint.
- Claude Desktop extension (`mcpb/`, manifest 0.3) whose launcher runs the server with `uvx` from
  the release's git tag, `server.json` registry metadata, and `scripts/cut-release.sh`.
