# LibreCAD-mcp Remote API

LibreCAD-mcp is an unofficial fork of [LibreCAD](https://github.com/LibreCAD/LibreCAD). It adds a
small HTTP server inside LibreCAD, the **Remote API**, so that programs on the same computer can
read and edit the drawings you have open. The main user is the `librecad-mcp` MCP server in
[`mcp/`](../../mcp/README.md): through it, Claude (or any other MCP client) can open DXF/DWG
files, draw, query, measure, render a picture of the drawing, and save or export it.

- The Remote API is **off by default**. Nothing listens until you enable it.
- It listens on **127.0.0.1 only**. No other computer can connect.
- Every request needs a **token**. Only programs that know it get in.
- It is built for **one client at a time**. Requests are carried out one after another, in the
  order they arrive; each connection is read on its own, so a slow or idle connection holds up
  nobody else.

The design and the full contract are in [DESIGN.md](DESIGN.md). This page is the user guide.

## Contents

1. [Enabling it](#1-enabling-it)
2. [Connecting Claude or another MCP client](#2-connecting-claude-or-another-mcp-client)
3. [Authentication](#3-authentication)
4. [The discovery file](#4-the-discovery-file)
5. [Trying it with curl](#5-trying-it-with-curl)
6. [Conventions](#6-conventions)
7. [Endpoints](#7-endpoints)
8. [Entity types](#8-entity-types)
9. [Errors](#9-errors)
10. [Dialogs, the command line and undo](#10-dialogs-the-command-line-and-undo)
11. [Limits and timeouts](#11-limits-and-timeouts)
12. [Security model](#12-security-model)
13. [Privacy](#13-privacy)
14. [Troubleshooting](#14-troubleshooting)

## 1. Enabling it

Open **Options > Application Preferences** and go to the **Remote API (MCP)** tab.

| Control | What it does |
|---|---|
| **Enable Remote API (local MCP server access)** | Starts the server when you press OK. Off by default. |
| **Port** | TCP port on 127.0.0.1, 1024 to 65535. Default **13140**. |
| **Token** | The secret clients must send. It is hidden until you press **Show**. **Copy** puts it on the clipboard. **Regenerate** makes a new one when you press OK, and clients holding the old token stop working. |
| **Copy MCP config** | Copies a ready `.mcp.json` snippet. It holds **no token** and no address: the MCP server reads both from the [discovery file](#4-the-discovery-file) on every call, so the snippet keeps working after **Regenerate** or a port change and can be committed with a project. A build of a source checkout runs the server from that checkout (`uv run --directory <checkout>/mcp`); a release build runs it from git at its own release tag. |
| **Show API activity in the command line** | Writes one `[MCP] ...` line to the command history for every change a client makes, for example `[MCP] created 4 entities on layer 'walls'`. |
| **Allow running command-line commands** | Lets clients type LibreCAD commands (`POST /command`). When off, that route answers `403 commands_disabled`. |
| **Restrict file access to these folders** | `;`-separated folders. When set, open, save-as and export only accept paths inside them. Empty means any absolute local path. |
| **Allow network (UNC) paths** | Off by default: `\\server\share\...` paths are refused (see [Security model](#12-security-model)). |
| Status | `Listening on 127.0.0.1:13140`, `Disabled`, or `Port 13140 is in use`, as it was when the tab was opened: reopen Preferences to see the effect of OK (with "Show API activity" on, the command line also says `[MCP] Remote API listening on 127.0.0.1:13140`). |

Changing Enabled, Port or Token restarts the server. The other options apply to the next request.

If the port is taken, for example by a second LibreCAD, the server does not start. The command
history says `[MCP] Remote API not started: port 13140 is in use (another LibreCAD?)`, the
Preferences tab shows the same, and no dialog interrupts you.

The settings live in the LibreCAD settings store (the registry on Windows, an INI file on Linux,
a plist on macOS), group `RemoteAPI`: `Enabled`, `Port`, `Token`, `Notify`, `AllowCommands`,
`AllowedRoots` and `AllowNetworkPaths`. Upstream LibreCAD ignores this group, so both can share
the same preferences. **Export** and **Import** in Application Preferences leave this group out:
an exported settings file never carries the token, and importing one never turns the API on.

## 2. Connecting Claude or another MCP client

On the same computer you normally configure **nothing**: the MCP server finds the address and
the token in the [discovery file](#4-the-discovery-file). Follow [mcp/README.md](../../mcp/README.md):

- **Claude Desktop:** install the `.mcpb` bundle from the
  [releases page](https://github.com/ShiftSad/LibreCAD-mcp/releases). It carries the server, so
  only `uv` is needed. Do not hand-edit `claude_desktop_config.json`.
- **Claude Code** and other clients, from a checkout of the fork (this works before any release is
  published; `uv` must be installed):

  ```sh
  claude mcp add librecad -- uv run --frozen --directory C:/path/to/LibreCAD-mcp/mcp librecad-mcp
  ```

  or the same as `"command": "uv", "args": ["run", "--frozen", "--directory", "<checkout>/mcp",
  "librecad-mcp"]` in `.mcp.json`. **Copy MCP config** writes this form for a build of a checkout.
- Once a release is published, the server can also run straight from git at its tag:

  ```json
  {
    "mcpServers": {
      "librecad": {
        "command": "uvx",
        "args": ["--from", "git+https://github.com/ShiftSad/LibreCAD-mcp@v2.2.2.6-mcp.1#subdirectory=mcp",
                 "librecad-mcp"],
        "env": {}
      }
    }
  }
  ```

The MCP server reads these environment variables. All are optional when LibreCAD runs on the
same computer.

| Variable | Default | Purpose |
|---|---|---|
| `LIBRECAD_API_URL` | from the discovery file, else `http://127.0.0.1:13140` | API base URL; leave it unset, or it overrides the port LibreCAD writes to the discovery file |
| `LIBRECAD_API_TOKEN` | from the discovery file | API token |
| `LIBRECAD_API_DISCOVERY` | the OS default path | an explicit path of `remote-api.json` |
| `LIBRECAD_API_TIMEOUT` | `30` | seconds per call; long operations use 50 (LibreCAD is told, see [section 11](#11-limits-and-timeouts)) |
| `LIBRECAD_MCP_ALLOWED_ROOTS` | unset | `os.pathsep`-separated folders the MCP server allows, checked before LibreCAD checks its own |
| `LIBRECAD_MCP_ALLOW_NETWORK_PATHS` | `0` | allow UNC paths on the MCP side |

A value that starts with `${` counts as unset, because Claude Desktop leaves unfilled optional
fields that way.

## 3. Authentication

The token is 32 lowercase hexadecimal characters (128 random bits). LibreCAD makes one the first
time you enable the API. If it cannot make one, the server does not start.

Send it on **every** request, `/status` included, in one of two headers:

```
X-Api-Token: 3f9c...e1
Authorization: Bearer 3f9c...e1
```

A request without a valid token gets:

```json
HTTP/1.1 401 Unauthorized
{"error": "unauthorized", "app": "LibreCAD"}
```

The `app` field lets a client tell that it reached LibreCAD and only the token is wrong. The
token is compared in constant time. LibreCAD never writes it to a log, the command history, or
any API answer.

## 4. The discovery file

While the server runs, LibreCAD keeps a small JSON file that tells local clients where it is:

| System | Path |
|---|---|
| Windows | `%APPDATA%\LibreCAD\LibreCAD-2\remote-api.json` |
| Linux | `~/.local/share/LibreCAD/LibreCAD-2/remote-api.json` |
| macOS | `~/Library/Application Support/LibreCAD/LibreCAD-2/remote-api.json` |
| Linux, the snap | `~/snap/librecad-mcp/current/.local/share/LibreCAD/LibreCAD-2/remote-api.json` (the snap's own home) |
| any, when `LIBRECAD_SETTINGS_DIR` is set | `<that folder>/remote-api.json` |

```json
{"url": "http://127.0.0.1:13140", "port": 13140, "token": "...", "pid": 1234,
 "app": "LibreCAD", "app_version": "2.2.2.6-alpha", "mcp_build": 1, "api_version": "1.0",
 "started_at": "2026-10-14T12:00:00Z"}
```

- It is written atomically and only your user can read it (mode 0600 on Linux and macOS; your
  per-user `%APPDATA%` on Windows).
- It is rewritten when the server restarts or the token changes, and removed when the server
  stops. If LibreCAD crashes or is killed, the file stays behind; its `pid` then names a process
  that no longer runs. The MCP server never uses such a stale file: it does not send its token
  anywhere (another program could hold the port by then, and LibreCAD keeps the token for its
  next start) and says that LibreCAD is not running.
- `GET /api/v1/status` reports the path as `discovery_file`, never the token.

The MCP server looks for its connection in this order: `LIBRECAD_API_URL` and
`LIBRECAD_API_TOKEN`; then `LIBRECAD_API_DISCOVERY`; then the default paths above (on Linux, the
snap's after the usual one).

`LIBRECAD_SETTINGS_DIR` is meant for tests and sandboxes: it moves all of LibreCAD's settings
into INI files in that folder, and the discovery file with them. An MCP server started next to
such a LibreCAD needs `LIBRECAD_API_DISCOVERY=<that folder>/remote-api.json`.

## 5. Trying it with curl

`curl` ships with Windows 10 and later (`curl.exe`), Linux and macOS. Take the token from the
Preferences tab or the discovery file. The quoting below is for a POSIX shell (bash, zsh, Git
Bash); `cmd.exe` and PowerShell quote JSON differently.

```sh
TOKEN=3f9c...e1
API=http://127.0.0.1:13140/api/v1

curl -s -H "X-Api-Token: $TOKEN" $API/status

# a new drawing, then a 100 x 50 rectangle and a circle in it (one undo step);
# use the document id the first call returns (2 here)
curl -s -H "X-Api-Token: $TOKEN" -X POST $API/documents/new -d '{}'
curl -s -H "X-Api-Token: $TOKEN" -X POST "$API/entities?doc=2" -d '{"entities": [
  {"type": "polyline", "vertices": [[0,0],[100,0],[100,50],[0,50]], "closed": true},
  {"type": "circle", "center": [150,25], "radius": 20}]}'

# a picture of the whole drawing
curl -s -H "X-Api-Token: $TOKEN" "$API/render?doc=2&width=800&height=600" -o drawing.png

# save it as a DWG for AutoCAD 2018-2026
curl -s -H "X-Api-Token: $TOKEN" -X POST "$API/documents/save_as?doc=2" \
     -d '{"path": "C:/Users/me/Documents/plan.dwg", "format": "dwg", "version": "2018"}'
```

`GET /api/v1/routes` lists every route the running build has, with its capability, timeout and
a one-line summary.

## 6. Conventions

| Topic | Rule |
|---|---|
| Prefix and version | Everything is under `/api/v1`. `/status` reports `"api_version": "1.0"`. New fields and routes keep v1; a breaking change would move to `/api/v2`. |
| Bodies | Request bodies are JSON objects. An empty body counts as `{}`. Answers are JSON (`application/json; charset=utf-8`), except `/render`, which returns `image/png`. |
| Unknown fields | Refused with `400 unknown_field`, never ignored, so a typo cannot pass silently. The answer names the `field` and lists the accepted ones in `allowed` (for `PATCH /entities/{id}`: that entity's geometry fields and `layer`, `color`, `linetype`, `lineweight`). Unknown query parameters are refused the same way on every route (`doc` is always accepted). |
| `null` | A JSON `null` counts as a missing field, at any depth (inside entity specs and hatch loops too): an optional field takes its default, a required one gives `400 missing_param`, and in a `PATCH` it changes nothing. |
| Documents | Routes that work on a drawing take `?doc=<id>`, the `id` from `/documents`. Ids start at **0** (`?doc=0` is the first drawing). Without `?doc=` they use the active drawing; `doc` given twice is `400 bad_param`. Print previews are never documents. |
| Units | Coordinates are in drawing units; `/drawing` says which (`"units": "mm"`). Lineweights are in mm. |
| Angles | Always **degrees**, counter-clockwise from +X, in fields that end in `_deg`. One exception: an ellipse's `start_angle_deg` and `end_angle_deg` are parametric angles measured from its `major_axis`, as DXF stores them ([section 8](#8-entity-types)). |
| Linetypes | Names are read in any case (`"Dashed"`, `"DASHED"`) and written in upper case, as in DXF (`"DASHED"`, `"CONTINUOUS"`). |
| Points | `[x, y]`. A third value is ignored on input. Invalid values come back as `null`. |
| Entity ids | `id` is a stable integer that survives edits, moves and undo. Copies get new ids. Only top-level entities of model space are addressable. |
| Query strings | Percent-decoded, `+` is a space. Repeat a key for several values (`type=line&type=arc`); `type` also accepts one comma-separated value. |
| Path parameters | Percent-encode them completely, for example a layer `A/B` as `/layers/A%2FB`. |
| Success | A route-specific object, with no wrapper. It may add `warnings` (list of strings), `skipped_dialogs` and `cancelled_action` ([section 10](#10-dialogs-the-command-line-and-undo)). |
| Undo | Answers that change a drawing say `"undoable": true` or `false`. |
| Headers | Every answer has `Cache-Control: no-store`, `X-Content-Type-Options: nosniff` and `Connection: close`. A client may send `X-Request-Timeout-Ms: <ms>`, how long it waits for the answer: LibreCAD then stops waiting a little before it does ([section 11](#11-limits-and-timeouts)). |

## 7. Endpoints

All paths start with `/api/v1`. "Doc" means the route takes `?doc=`. The **capability** column is
the name `/status` lists in `capabilities` when the build has the route.

### Application and files

| Method and path | Capability | What it does |
|---|---|---|
| `GET /status` | status | App and API version, `mcp_build`, platform, capabilities, DWG support, open documents, active document, whether a dialog is open. Answers even while you have a dialog open. |
| `GET /routes` | routes | Every route with method, path, capability, timeout and summary. |
| `GET /formats` | documents | File types LibreCAD opens; the DXF and DWG versions it saves, each with a `label` (`"AutoCAD 2018-2026 (AC1032)"`), the default, the `extension` and `aliases` (AutoCAD codes such as `AC1032`, `R2000` spellings, and `2019` to `2026` for `2018`); export formats. |
| `GET /documents` | documents | Open drawings: id, name, path, modified, active, format and version, entity count. |
| `POST /documents/new` | documents | New drawing from your default template, not marked modified. `{activate?}` → `201 {document}`. |
| `POST /documents/open` | documents | `{path, accept_partial?}` → `201 {document, warnings}`, or `200 {document, already_open: true}` when it is open already. DXF, DWG, LFF, CXF, JWW, SHP. |
| `POST /documents/activate` (doc) | documents | Brings the drawing to the front. |
| `POST /documents/save` (doc) | documents | Saves in place in the drawing's own format → `{document, path, format, version, label, left_out}`. Never asks for a file name: an unnamed drawing gives `409 document_unnamed`. The folder restrictions apply to the drawing's own file too; a read-only file gives `500 save_failed`. |
| `POST /documents/save_as` (doc) | documents | `{path, format: "dxf"\|"dwg", version?, overwrite?}` → `{document, path, format, version, label, left_out}`. The window then shows the new file, as with File > Save As. |
| `POST /documents/export` (doc) | export | Writes a **copy** as DXF, DWG (any version), PDF, PNG or SVG, and never renames the drawing → `{path, format, bytes, left_out}`, plus `version` and `label` for DXF/DWG. PNG/SVG take `width`, `height`, `margin_px`, `background`, `monochrome`; PDF uses the drawing's print settings and `fit_to_page`. The drawing's own file, or one another open drawing uses, gives `409 file_open_in_other_window`. |
| `POST /documents/close` (doc) | documents | `{discard_changes?}`. A modified drawing gives `409 unsaved_changes` unless `discard_changes` is true. Never asks. |

### Drawing, view and command line

| Method and path | Capability | What it does |
|---|---|---|
| `GET /drawing` (doc) | drawing | Units, extents, format and DWG/DXF version, counts by entity type, layer and block counts, current layer, undo state, a few header variables. |
| `GET /render` (doc) | render | PNG of the drawing: `mode=extents\|view\|window`, `min`/`max` for a window, `width`, `height`, `margin_px`, `background`, `monochrome`. Headers `X-LibreCAD-Window` (the rendered area), `X-LibreCAD-Scale` (pixels per unit) and, for an empty drawing, `X-LibreCAD-Empty: 1`. Your view on screen does not move. |
| `GET /view` (doc) | view | The window you see: corners, center, size in pixels, pixels per unit. |
| `POST /view` (doc) | view | Changes your view: `extents`, `window`, `zoom_in`, `zoom_out`, `pan`, `previous`, `named` (model-space views only). A field the action does not use gives `400 bad_param`. Does not edit the drawing. |
| `POST /undo`, `POST /redo` (doc) | undo | `{steps?}`, like Ctrl+Z and Ctrl+Shift+Z. Layer and block-list changes are not undone (LibreCAD does not record them). |
| `POST /command` (doc) | command | Types commands and inputs into the command line: `{command: "line", inputs: ["0,0", "100,0", "escape", "escape"]}` (one `escape` ends the segment chain, the second leaves the tool). Returns the new history lines, the prompt and the active tool (`active_action`, `null` once no tool is active). |
| `GET /commands` | command | LibreCAD's main command names (they may be in your LibreCAD language), such as `line2p`. The short aliases (`line`, `li`, `l`, ...) work in `POST /command` but are not listed. |

### Layers and blocks

Layer changes are **not undoable** in LibreCAD; the answers say `"undoable": false`.

| Method and path | Capability | What it does |
|---|---|---|
| `GET /layers` (doc) | layers | Layers with color, linetype, lineweight, frozen, locked, printable, construction, current, entity count. |
| `POST /layers` (doc) | layers | `{name, color?, linetype?, lineweight?, frozen?, locked?, printable?, construction?, make_current?}` → `201`. The current layer stays as it was unless `make_current` is true. A layer's lineweight is `"Default"` or a width in mm (never `ByLayer`/`ByBlock`). An existing name gives `409 layer_exists`. |
| `PATCH /layers/{name}` (doc) | layers | Rename, restyle, freeze or lock. Layer "0" cannot be renamed. |
| `POST /layers/{name}/current` (doc) | layers | Makes the layer current. |
| `DELETE /layers/{name}` (doc) | layers | `?entities=refuse\|delete\|move&move_to=0` → `{deleted: name, entities_deleted, entities_moved, undoable: false}`. By default a layer that still has entities is refused (`409 layer_not_empty`). A locked layer that still has entities, or a locked `move_to` layer, gives `409 layer_locked`. |
| `GET /blocks` (doc) | blocks | Block definitions with base point, entity count and number of inserts. |
| `POST /blocks/{name}/insert` (doc) | blocks | `{position, scale?, angle_deg?, layer?, columns?, rows?, spacing?}` → `201 {entity}`. A negative scale mirrors the block; zero is refused. |

### Entities, selection and measuring

Each request that changes entities is **one undo step**.

| Method and path | Capability | What it does |
|---|---|---|
| `GET /entities` (doc) | entities | Filters `type`, `layer`, `bbox=x1,y1,x2,y2` with `bbox_mode=crossing\|window`, `selected`, `include_hidden`; paging `limit` (up to 1000) and `offset`; `detail=summary\|full`. Returns `{entities, total, offset, limit, next_offset}`. A `layer` that does not exist gives `404 unknown_layer`. |
| `GET /entities/{id}` (doc) | entities | One entity in full detail. |
| `POST /entities` (doc) | entities | `{entities: [spec, ...], layer?, select?}`, 1 to 5000 specs. **All or nothing**: if one spec is wrong, nothing is created and one `422 invalid_entity` lists every bad field in `errors` (and the accepted values in `allowed` where there is a list); an `insert` of an unknown block gives `404 unknown_block`. Returns `201 {created, ids, undoable: true}`. |
| `PATCH /entities/{id}` (doc) | entities | Changes attributes or geometry fields of one entity; the id stays. What the body does not name stays exactly as it was (angles too). A field of another type gives `400 unknown_field` with the accepted fields in `allowed`. A spline read from a file keeps its own knot vector and weights when only its control points move; a new degree, closed flag or number of control points for such a spline, and any geometry change of a polyline with `elliptic_segments`, give `422 invalid_entity`. |
| `DELETE /entities/{id}`, `POST /entities/delete {ids}` (doc) | entities | Deletes; if one id is unknown or locked, nothing is deleted. |
| `POST /entities/transform` (doc) | transform | `move` (`offset`), `rotate` (`center`, `angle_deg`), `scale` (`base`, `factor` > 0, or `[fx, fy]`; to flip, use `mirror`), `mirror` (`axis`); `copy` and `copies` make copies with new ids. If one entity cannot be transformed, nothing changes: `422 invalid_entity` with `errors` by id. That covers a block reference LibreCAD cannot edit, and a scale with `fx != fy` of text, mtext, a polyline with arc segments, a hatch with curved edges, or a radial or diametric dimension, none of which can follow it. |
| `GET /selection`, `POST /selection` (doc) | selection | Reads or changes the selection you see: `set`, `add`, `remove`, `clear`, `all`. `ids` and the filters (`type`, `layer`, `bbox`) combine: an entity must match all that are given. `set`, `add` and `remove` need `ids` or a filter (else `400 missing_param`); `all` applies the filters to every entity. |
| `POST /measure` (doc) | measure | `distance` → `{distance, dx, dy, angle_deg}` (or `{distance, nearest_point}` to an entity); `length` → `{length, per_entity}`, for curves only (lines, arcs, circles, ellipses, polylines, splines: anything else is `400 bad_param` with `ids`); `area` → `{area, perimeter, per_entity}` of closed entities and hatches (holes included in the perimeter), or of `points`; `angle` → `{angle_deg}`; `intersections` → `{points}`. Read-only. |

## 8. Entity types

Specs (input) and entities (output) use the **same field names**. Output adds `id`, `type`,
`layer`, `color`, `linetype`, `lineweight`, `handle`, `visible`, `locked`, `selected`, `bbox`, and
read-only values such as `length` or `area`.

| `type` | Fields (defaults) |
|---|---|
| `line` | `start`, `end` |
| `circle` | `center`, `radius` > 0 |
| `arc` | `center`, `radius` > 0, `start_angle_deg`, `end_angle_deg`, `reversed: false` |
| `ellipse` | `center`, `major_axis` (a vector), `ratio` in (0, 1], `start_angle_deg: 0`, `end_angle_deg: 360`, `reversed: false`. The angles are **parametric** (eccentric anomaly), counter-clockwise from `major_axis`, as DXF stores them: the point at `t` is `center + major_axis·cos t + minor_axis·sin t`. They are polar angles only for a circle-like `ratio` of 1. |
| `point` | `position` |
| `polyline` | `vertices: [[x, y] or [x, y, bulge], ...]` (at least 2), `closed: false`. A bulge under 1e-8 is a straight segment. |
| `spline` | either `control_points` with `degree: 3` (1 to 3), or `fit_points`; `closed: false` |
| `text` | `position`, `text`, `height` > 0, `angle_deg: 0`, `halign`, `valign`, `style: "standard"`, `width_factor: 1`; `second_point` (the end of the baseline) is required when `halign` is `aligned` or `fit` |
| `mtext` | `position`, `text`, `height` > 0, `width: 0`, `angle_deg: 0`, `attachment`, `line_spacing: 1`, `style` |
| `dimension` | `dim_type`: `linear` (`p1`, `p2`, `line_point`, `angle_deg`), `aligned`, `radial`, `diametric` or `angular`; `text` (empty = measured value) |
| `hatch` | `pattern: "SOLID"`, `scale: 1`, `angle_deg: 0`, and `boundary` (point loops) or `boundary_ids` (closed entities) |
| `insert` | `block`, `position`, `scale: 1` or `[sx, sy]` (negative mirrors, zero is refused), `angle_deg: 0`, `columns`, `rows`, `spacing` |

Any spec may also set `layer`, `color` (`"ByLayer"`, `"ByBlock"`, `"#rrggbb"`, or an ACI number
1 to 255), `linetype` (`"ByLayer"`, `"CONTINUOUS"`, `"DASHED"`, ...; any case) and `lineweight` (`"ByLayer"`,
`"ByBlock"`, `"Default"`, or mm such as `0.25`). Without them the entity takes its layer's
properties. Lineweights must be one of the standard widths (0.25, 0.35, 0.5, ...); any other value
is refused and the answer lists the accepted ones. Images, leaders, multileaders, solids,
parabolas and hyperbolas are listed read-only. The complete table is in
[DESIGN.md section 2.6](DESIGN.md#26-entity-specs-and-ids).

Read back, an `insert`'s `scale` is always `[sx, sy]`, a full ellipse has angles 0 and 360 (also
one a file stores with equal start and end angles), and angles are in [0, 360). A closed polyline
lists every vertex, also the last one when LibreCAD leaves out a closing segment shorter than
1e-4, and its `area` and `length` include that closing segment. A polyline LibreCAD has turned
elliptic (a non-uniform scale in the GUI) reports `"elliptic_segments": true`: its bulges describe
circular arcs only. A hatch's `loop_count` counts every boundary loop, holes included.

## 9. Errors

Every error has the same shape:

```json
{"error": "unknown_entity", "detail": "no entity with id 42", "hint": "...", "ids": [42]}
```

`error` is a stable token to test in code; `detail` and `hint` are for people; other fields give
context (`param`, `allowed`, `path`, `ids`, `name`, `errors`). A bare `404 not_found` always means
**the route does not exist** in this build; a missing drawing, entity, layer or file has its own
token.

`422 invalid_entity` maps each bad field to the reason, and, where LibreCAD has a list of accepted
values, maps the field to that list:

```json
{"error": "invalid_entity", "detail": "1 of 2 entity specs are invalid; nothing was created",
 "errors": {"entities[1].lineweight": "not a standard lineweight; ..."},
 "allowed": {"entities[1].lineweight": ["ByLayer", "ByBlock", "Default", 0, 0.05, ...]}}
```

`ambiguous_id` (409) means two entities share one id, which happens when a block reference is
copied in LibreCAD without a new identity; find them by area or layer instead. `too_many` (422)
means more than 5000 entities or ids in one request.

| HTTP | Tokens |
|---|---|
| 400 | `bad_request`, `invalid_json`, `body_must_be_object`, `unknown_field`, `missing_param`, `bad_param`, `bad_path` (also a path with a control character) |
| 401 | `unauthorized` |
| 403 | `origin_not_allowed`, `path_not_allowed`, `commands_disabled` |
| 404 | `not_found` (route), `unknown_document`, `unknown_entity`, `unknown_layer`, `unknown_block`, `unknown_view`, `file_not_found` |
| 405 | `method_not_allowed` (with `allowed`) |
| 408, 411, 413, 421, 431 | `request_timeout`, `length_required`, `body_too_large`, `bad_host`, `headers_too_large` |
| 409 | `no_document`, `gui_modal_open`, `unsaved_changes`, `file_exists`, `file_open_in_other_window`, `document_unnamed`, `cannot_save_in_place`, `partial_import`, `document_closed` (the window of a drawing being opened was closed meanwhile), `layer_exists`, `layer_not_empty`, `layer_locked`, `layer_frozen`, `entity_locked`, `ambiguous_id` |
| 422 | `invalid_entity` (with `errors`, for example `{"entities[2].radius": "must be > 0"}`, and `allowed`), `unsupported_format`, `extension_mismatch`, `import_failed`, `invalid_hatch_boundary`, `not_closed`, `cannot_rename_layer_0`, `cannot_delete_layer_0`, `too_many`, `bad_param` (an unknown save or export `version`, with `allowed`) |
| 500 | `internal`, `save_failed`, `export_failed` |
| 501 | `dwg_unsupported` (a build without DWG support) |
| 503 | `shutting_down`, `server_busy` (more than 16 connections at once: retry) |
| 504 | `ui_timeout` (with `started`) |

## 10. Dialogs, the command line and undo

**No dialogs.** Nothing an API request does opens a dialog: open, save, save-as, export and close
are written so they never ask. As a safety net, while a request runs, any dialog that still pops
up (for example from a typed command) is closed at once, and the answer lists it:

```json
"skipped_dialogs": [{"class": "QMessageBox", "title": "...", "text": "..."}]
```

A PNG from `/render` cannot carry that field, so it lists closed dialogs in the
`X-LibreCAD-Skipped-Dialogs` header instead.

**Your dialogs come first.** If you have a dialog open in LibreCAD (Preferences, a file dialog, a
message), requests do not wait for it: every route, read-only ones included, answers
`409 gui_modal_open` with the window title. Close it and the client can go on. `/status` still
answers and says `"modal_open": true` and `"modal_window": "<title>"`. While you have a context
menu open, drag something, or LibreCAD is busy with your own action (opening a file, closing
drawings, editing a block, quitting), a request waits until that is over instead of running in
the middle of it. A request never processes your keyboard or mouse input while it runs, so it
can neither swallow a dialog you open nor close a window under you.

**Your tool is cancelled.** A request that changes a drawing first cancels the tool you were using
(for example a half-drawn line), and the answer says which in `cancelled_action`. `/command` is
the exception: it types into the tool that is active.

**Undo.** Each request that changes entities is one undo step, so Ctrl+Z in LibreCAD (or
`POST /undo`) takes back exactly what the client did. Layer and block-list changes cannot be
undone in LibreCAD.

**Slow work.** If LibreCAD does not answer in time (a huge drawing, or LibreCAD is busy), the
request answers `504 ui_timeout`. With `"started": false` nothing was done; with `true` the work
may still finish afterwards, and the command history then says `[MCP] ... finished after the
client gave up`. Check with `/documents` or `/entities` before repeating it. A request whose
client has given up (closed the connection, or passed its `X-Request-Timeout-Ms`) before
LibreCAD got to it never runs at all. Autosave waits while a request is running.

## 11. Limits and timeouts

| Limit | Value | Error |
|---|---|---|
| request line and headers | 16 KiB | `431 headers_too_large` |
| body | 4 MiB | `413 body_too_large` |
| `Transfer-Encoding: chunked` | refused | `411 length_required` |
| request line and headers arriving | 5 s | `408 request_timeout` |
| reading the whole request | 15 s | `408 request_timeout` |
| connections read at the same time | 16 | `503 server_busy` |
| entities per `POST /entities`, ids per request | 5000 | `422 too_many` |
| `GET /entities` page | 1000 | |
| render size | 64 to 4096 pixels per side | |
| copies per transform | 1 to 100 | |

| Routes | LibreCAD waits up to |
|---|---|
| most routes | 10 s |
| `/status` | 5 s |
| `/render`, `/command` | 30 s |
| new, open, save, save as, export | 120 s |

A client's `X-Request-Timeout-Ms` header caps these: LibreCAD stops waiting at the client's
deadline (counted from when the request arrived) and answers `504 ui_timeout`, so the client hears
whether the work started instead of timing out blind. The MCP server waits 30 s per call (50 s for
new, open, save, save as and export: MCP clients such as Claude Desktop stop a tool after about a
minute) and sends its deadline less a margin of up to 2 s.

## 12. Security model

Anyone who has the token can do, in your open drawings, what you can do, and can read and write
drawing files wherever LibreCAD may (limited by the folder restriction below). Treat the token like
a password. The rules:

1. **Off by default; loopback only.** The server binds `127.0.0.1`. There is no LAN mode.
2. **Token on every request**, compared in constant time; regenerate it in Preferences if it
   leaked.
3. **No browsers.** A request with an `Origin` header is refused (`403 origin_not_allowed`), no
   CORS headers are ever sent, and `OPTIONS` gives `405`. The `Host` header must be
   `127.0.0.1:<port>`, `localhost:<port>` or `[::1]:<port>` (`421 bad_host`), which defeats DNS
   rebinding. A web page you visit therefore cannot use the API, even if it guessed the port.
4. **File paths** for open, save-as and export must be absolute, without control characters (a
   NUL would cut the name the system opens: `run.bat\0.dxf` writes `run.bat`) and, on Windows,
   without a `:` after the drive (`host.txt:x.pdf` is a hidden stream of `host.txt`) or a device
   name such as `COM1.dxf` or `NUL.svg`; they are
   normalized first. Network paths (`\\server\share`, `\\?\`, device paths, `//host`) are
   refused unless you allow them, because opening one can send your Windows credentials to that
   server. With **Restrict file access** set, only paths inside those folders are accepted
   (`403 path_not_allowed`), checked after resolving symbolic links and, on Windows, directory
   junctions and mount points, so no link inside an allowed folder leads out of it. The file
   extension must match the format, an existing file is never replaced without `"overwrite": true`
   (`409 file_exists`), and a file open in another LibreCAD window is never written.
5. **Destructive actions need an explicit flag**: `discard_changes` to close a modified drawing,
   an `entities` policy to delete a layer that has entities, `overwrite` to replace a file. The MCP
   server also asks you before it sets them.
6. **Command line** access can be switched off (`403 commands_disabled`).
7. **Audit trail.** With "Show API activity" on, every change writes an `[MCP] ...` line to the
   command history. Closing with discarded changes, deleting a layer and overwriting a file are
   always written, whatever that option says.
8. **Limits** on request size and time (section 11) keep a broken client from hanging LibreCAD,
   and each connection is read on its own, so an idle or slow one blocks no other client.

What this does **not** protect against: a program that already runs under your user account can
read the discovery file or your settings and so the token, just as it could read your drawings
directly. The Remote API adds no new way in for other users of the computer or for other
computers.

## 13. Privacy

- The Remote API sends nothing anywhere by itself. It only answers requests from programs on your
  computer, and it writes nothing about them except the audit lines in the command history.
- What a client does with the answers is up to the client. When you use Claude through the MCP
  server, the drawing data the model asks for (entity lists, measurements, rendered pictures, file
  paths) goes to Anthropic like any other conversation content, under the terms of your Claude
  account. Other MCP clients send it to their own model provider.
- LibreCAD-mcp checks for new versions on GitHub (`ShiftSad/LibreCAD-mcp` releases) when the
  usual LibreCAD update check is on (Options > Application Preferences). Turn it off there if you
  prefer.

## 14. Troubleshooting

| Symptom | What to do |
|---|---|
| The MCP server says LibreCAD is not reachable | Is LibreCAD running, and is the Remote API enabled? Look at the status line of the Remote API tab. |
| `401 unauthorized` | The client has an old token. Remove `LIBRECAD_API_TOKEN` from its configuration so it reads the discovery file, or copy the current token again. |
| "the Remote API discovery file ... was left by a LibreCAD that is no longer running" | LibreCAD crashed or was killed. Start it again: it rewrites the file. |
| `503 server_busy` | More than 16 connections at once. Retry; if it persists, a program keeps connections open to the port. |
| `Port 13140 is in use` | Another LibreCAD (or another program) has the port. Close it or choose another port; the discovery file always holds the right one. |
| `409 gui_modal_open` | Close the dialog that is open in LibreCAD. |
| `504 ui_timeout` | LibreCAD was busy. Check with `/documents` or `/entities` whether the change happened before repeating it. |
| `403 path_not_allowed` | The path is outside the folders allowed in the Remote API tab (or in `LIBRECAD_MCP_ALLOWED_ROOTS`), or it is a network path. |
| `403 commands_disabled` | Turn on "Allow running command-line commands". |
| `422 invalid_entity` on text | `aligned` and `fit` text need `second_point`. If the error names `style`, LibreCAD found no font at all; check Options > Application Preferences > Paths (an unknown style name alone only adds a warning: LibreCAD uses another font). |
| `404 not_found` on a route the docs list | The LibreCAD build is older than the MCP server; compare `capabilities` in `/status`. |

The end-to-end test in [`tools/remote-api/`](../../tools/remote-api/README.md) exercises every
point of this page against a real LibreCAD.
