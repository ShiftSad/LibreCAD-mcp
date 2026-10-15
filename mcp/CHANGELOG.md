# Changelog

All notable changes to the LibreCAD MCP server are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Security
- `delete_layer` finds the layer as LibreCAD does (Unicode NFC) and asks about, then deletes, that
  layer by its listed name: another spelling of the name no longer skips the confirmation. An
  unknown name gives `unknown_layer` without a delete.
- A discovery file left by a LibreCAD that no longer runs is never used, so its token is not sent
  to whatever listens on that port now. Its process must also be the LibreCAD that wrote it (a
  LibreCAD program that started before the file's `started_at`): a pid the system handed to
  another program does not count.
- A confirmed discard, layer delete or overwrite acts on the drawing (and the layer, with as many
  entities) the user was asked about; when it changed while the question was open, nothing
  happens (`not_confirmed`). The `user_confirmed` path names the drawing's `document_id`, and
  `close_document`/`delete_layer` then need it.
- Paths with control characters (a NUL cuts the name the system opens) are refused, and so are
  `\\?\` and `\\.\` paths even with `LIBRECAD_MCP_ALLOW_NETWORK_PATHS=1`, and a `:` stream in a
  network path.
- A save or export path that reaches an existing file is judged by that file's own name too: a
  Windows short name (`KEEPME~1.DXF` of `keepme.dxfold`) or a symbolic link no longer passes the
  extension rule for a file of another type, and the overwrite question names the file on disk.
- `COM¹`..`COM³` and `LPT¹`..`LPT³` are refused as device names, like `COM1`, and so are names with
  spaces or dots before the extension (`COM1 .dxf`).
- On Windows a path for open, save or a DXF/DWG export with a character outside the system's ANSI
  code page is refused (`bad_path`), as LibreCAD now refuses it: LibreCAD hands drawing file names
  to Windows in that code page, which turns a full-width `＼` into `\` (a path out of
  `LIBRECAD_MCP_ALLOWED_ROOTS`) and `ł` into `l` (another file). PDF, PNG and SVG exports take any
  name.

### Changed
- Every request tells LibreCAD the client's deadline (`X-Request-Timeout-Ms`, a whole number of ms,
  at most 600000, which LibreCAD now requires): LibreCAD answers `ui_timeout` (with `started`)
  before the call gives up, and never runs it afterwards.
- Text specs: `aligned` text takes its height and angle from `position` and `second_point` and
  `fit` text its angle, so `height` is optional for `aligned` and those fields are refused, as is a
  `width_factor` for both and a `valign` other than `baseline` for `aligned`, `fit` and `middle`
  (LibreCAD would drop them; it refuses them too).
- `create_layer`/`update_layer` take a layer lineweight (`"Default"` or mm); `set_view` takes a
  factor > 1; `insert_block` (and `insert` specs) accept a negative, mirroring scale.
- The `.mcpb` carries the server and runs it before falling back to git; `LIBRECAD_MCP_SOURCE`
  overrides both. The documentation runs it from a checkout until a release tag is published;
  `scripts/check-release.sh` is the release gate. The package and the extension carry the full
  GPL text.
- Docstrings, the guide and the prompts: `select_entities` combines ids and filters (AND);
  `run_command` needs `escape` twice to leave a drawing tool; `measure` lists its reply shapes and
  measures length of curves only; `close_document` says this server asks, not LibreCAD; the
  `convert-file` prompt leaves a drawing open that was already open; ellipse angles are parametric;
  linetypes come back in upper case.
- The snap's discovery file (`~/snap/librecad*/current/...`) is found on Linux.

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
