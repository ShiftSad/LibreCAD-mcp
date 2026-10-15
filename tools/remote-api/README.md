# Remote API test tools

Tools to check the LibreCAD-mcp Remote API ([docs](../../docs/remote-api/README.md),
[design](../../docs/remote-api/DESIGN.md)) against a real LibreCAD. Nothing here is part of the
LibreCAD build.

| File | What it does |
|---|---|
| `e2e_smoke.py` | End-to-end smoke test (DESIGN.md section 5.3). Starts LibreCAD with throw-away settings, or attaches to a running one, and walks through the whole API: documents, layers, entities, query, measure, transform, undo and redo, render, save as DXF, export to DWG, PDF, SVG and PNG, reopen, command line, and the error cases. Optionally drives the `librecad-mcp` MCP server too. Python 3.9 or newer, standard library only. |

## e2e_smoke.py

### Run it against a build

From the repository root. Each check prints `PASS` or `FAIL`; the exit code is 0 when all
passed and 1 otherwise (2 for bad arguments).

```sh
# Windows, development build (Qt is not deployed next to the exe: pass its bin folder)
py -3 tools/remote-api/e2e_smoke.py --exe C:/path/to/build/LibreCAD.exe --qt-bin C:/Qt/6.9.3/msvc2022_64/bin

# Windows, installed or portable LibreCAD-mcp
py -3 tools/remote-api/e2e_smoke.py --exe "C:/Program Files/LibreCAD/LibreCAD.exe"

# Linux
python3 tools/remote-api/e2e_smoke.py --exe ./build/librecad

# also through the MCP server (needs uv; runs `uv run --project mcp librecad-mcp`)
py -3 tools/remote-api/e2e_smoke.py --exe ... --via-mcp
```

What happens:

1. A temporary folder gets `settings/LibreCAD/LibreCAD-2.ini` with the first-start dialog, the
   splash and the update check turned off and the Remote API enabled on a free port with a random
   token (section `[RemoteAPI]`). Its `[Paths]` section sets `OtherSettingsDir` to the same
   folder, so the command alias and workspace files LibreCAD writes stay there too. A development
   build has no `resources/fonts` next to the executable: `Fonts` and `Patterns` then point at
   this checkout's `librecad/support/fonts` and `patterns` (text needs the font "standard").
2. LibreCAD starts with `LIBRECAD_SETTINGS_DIR=<tmp>/settings`, so it reads those settings
   instead of yours and writes its discovery file to `<tmp>/settings/remote-api.json`, and with
   `QT_QPA_PLATFORM=offscreen`, so no window appears. If a deployed package has no
   `platforms/qoffscreen` plugin, the test runs visibly instead. The script waits up to 60 s for
   the discovery file and `/status`.
3. The scenario runs, step by step. Files go to `<tmp>/out`.
4. LibreCAD is terminated. Its discovery file must then be gone, or stale (its `pid` no longer
   runs: a terminated process cannot remove it).
5. The temporary folder is deleted, unless `--keep`. On a failure the tail of LibreCAD's output
   (`<tmp>/librecad.log`) is printed.

Your own LibreCAD settings, drawings and discovery file are never touched in this mode.

### Options

| Option | Meaning |
|---|---|
| `--exe PATH` | LibreCAD executable to start. Default: `$LIBRECAD_EXE`. |
| `--qt-bin DIR` | Folder put first on `PATH` for LibreCAD, for a development build whose Qt DLLs are not next to the exe. |
| `--visible` | Show the window instead of `QT_QPA_PLATFORM=offscreen`. |
| `--keep` | Keep the temporary folder (settings, `librecad.log`, the exported files, the MCP server's stderr). |
| `--port N` | Remote API port. Default: a free one. |
| `--start-timeout S` | Seconds to wait for the API after start. Default 60. |
| `--attach` | Test a LibreCAD that is already running instead of starting one (see below). |
| `--discovery PATH` | With `--attach`: the `remote-api.json` to use. |
| `--via-mcp` | After the HTTP scenario, start the MCP server over stdio and call `get_status`, `add_entities` and `render_view` through it. |
| `--mcp-project DIR` | Project folder for `uv run --project DIR librecad-mcp`. Default: `mcp/`. |
| `--mcp-command CMD` | Start the MCP server with this command line instead, for example `C:/venv/Scripts/librecad-mcp.exe`. |
| `-v`, `--verbose` | Also print the details of passing checks. |
| `--selftest` | Check the script itself (see below). |

### --attach

`--attach` uses a LibreCAD you started yourself, with the Remote API enabled. It finds it like the
MCP server does: `LIBRECAD_API_URL` and `LIBRECAD_API_TOKEN`, else `--discovery` or
`LIBRECAD_API_DISCOVERY`, else the default discovery file of your system.

The scenario creates its own drawing and addresses every request with `?doc=`, so your other
drawings are not touched. At the end it closes the drawings it opened, discarding their changes.
Its files go to a temporary folder: if "Restrict file access to these folders" is set in your
Remote API preferences, the file steps fail with `403 path_not_allowed`.

### What it checks

The numbered steps follow DESIGN.md section 5.3:

| Step | Checks |
|---|---|
| 0 | The discovery file has the configured port and token, the started process's pid, and the documented keys. |
| 1 | `/status` (app, `api_version` 1.0, `mcp_build`, every capability, no modal dialog, `discovery_file`, no token, the common headers); `/routes` lists every route of DESIGN.md 2.4 with all its fields; `/formats`. |
| 2 | `POST /documents/new` gives an active, unnamed document. |
| 3 | Layer "walls" in red, reported as not undoable; the same name again gives `409 layer_exists`. |
| 4 | A closed 100 x 50 polyline, a circle, a text and a linear dimension in one batch (ids, types, layer, the dimension measures 100); a SOLID hatch from `boundary_ids`; an insert of a missing block gives `404 unknown_block` and creates nothing else of its batch; `/blocks/{name}/insert` gives `404 unknown_block`; radius 0 gives `422 invalid_entity` with `errors["entities[0].radius"]`. |
| 5 | `?type=polyline` and `?layer=walls` filters, `vertex_count` in summary detail, the full polyline and its bbox. |
| 6 | Area 5000 and perimeter 300 of the polyline; the distance 3-4-5. |
| 7 | A move with `copy` gives a new id at the right place and keeps the original; undo removes it (the copy's id is then `404 unknown_entity`), redo brings it back; a plain move keeps the id. |
| 8 | `/render` gives a PNG of the requested size, a window that covers the drawing and a scale; `mode=view` works too. |
| 9 | `save_as` DXF (the window now refers to the file, unmodified); `export` DWG 2018 and its alias 2026 (`AC1032` at offset 0), PDF (`%PDF`), SVG (`<svg`) and PNG (signature and size); the reported byte counts; exports do not rename or modify the drawing. Without DWG support: `501 dwg_unsupported`. |
| 10 | `close`; `open` of the DWG (`201`, then `200 already_open`); `/drawing` reports DWG version 2018 and the entities by type; layer "walls" survived. |
| 11 | `/command` `line`, `0,0`, `10,10`, `escape` adds one entity and prints output; an empty command gives `400 bad_param`; `/commands`. |
| 12 | No token or a wrong one: `401`; Bearer works; an unknown route, a trailing `/` and `/statuses`: bare `404 not_found`; a wrong method: `405`; `OPTIONS`: `405`; `Origin`: `403`; a foreign `Host`: `421`; unknown document and entity ids; a relative path: `400 bad_path`; a missing file: `404 file_not_found`; an existing file without `overwrite`: `409 file_exists`, file untouched; `{"bogus": 1}`: `400 unknown_field`. |
| MCP | With `--via-mcp`: `initialize`; `tools/list` has every tool of DESIGN.md 4.4; `get_status` reaches LibreCAD and shows no token; `add_entities` creates a line that the HTTP API then finds; `render_view` returns a PNG image; nothing but JSON-RPC on the server's stdout. |
| 13 | No answer of the whole run carried `skipped_dialogs` (no hidden dialog), `ui_timeout`, or the token. |
| 14 | After LibreCAD is stopped, the discovery file is gone or stale. |

A failing check prints the request and the answer it got, so a contract difference between the
app and DESIGN.md section 2 shows up directly.

The MCP part speaks MCP with a small built-in JSON-RPC client over stdio, so the script needs no
`mcp` package; the server it starts does need `uv` (or `--mcp-command`).

### --selftest

```sh
py -3 -I tools/remote-api/e2e_smoke.py --selftest
```

Runs the script's own HTTP client, file checks, settings and discovery handling, start-up polling
and MCP client against a local `http.server` stub and a stub MCP server. No LibreCAD is needed;
it takes a few seconds. CI runs it on Linux and Windows
([`mcp-tests.yml`](../../.github/workflows/mcp-tests.yml)).
