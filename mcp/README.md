# LibreCAD MCP

Let Claude, or any other MCP client, work a **running LibreCAD**: open DXF and DWG files, draw,
manage layers, query and measure geometry, look at the drawing, undo, and save or export. Every
change happens in the open LibreCAD window, where you see it at once.

`librecad-mcp` is a small [Model Context Protocol](https://modelcontextprotocol.io) server. It
talks to the **Remote API** of the [LibreCAD-mcp](https://github.com/ShiftSad/LibreCAD-mcp) build,
an unofficial fork of LibreCAD. Nothing leaves your computer through it:

- The Remote API is **off by default**. You turn it on in LibreCAD's Preferences.
- It listens on `127.0.0.1` only, never on the network, and every request needs a random token.
- This server talks to that local API and to nothing else. Your drawings stay on your machine.
  The AI model comes from your MCP client, and your conversation goes wherever that client sends it.

> This is not an official LibreCAD project. Please report problems
> [to the fork](https://github.com/ShiftSad/LibreCAD-mcp/issues), not to upstream LibreCAD.

## What it can do

| Group | Tools |
|---|---|
| Session | `get_status`, `list_formats`, `list_documents` |
| Drawings | `new_document`, `open_document`, `activate_document`, `save_document`, `save_document_as`, `export_document` (DXF R12..2018, DWG 2000..2018 where 2018 is AutoCAD 2018-2026, PDF, PNG, SVG), `close_document` |
| Looking | `get_drawing_info`, `render_view` (a PNG the AI can see), `get_view`, `set_view` |
| Layers and blocks | `list_layers`, `create_layer`, `update_layer`, `set_current_layer`, `delete_layer`, `list_blocks`, `insert_block` |
| Entities | `query_entities`, `get_entity`, `add_entities` (lines, circles, arcs, ellipses, points, polylines, splines, text, mtext, dimensions, hatches, inserts), `update_entity`, `delete_entities`, `transform_entities` (move, rotate, scale, mirror, copies), `select_entities`, `measure` |
| History and commands | `undo`, `redo`, `run_command` (LibreCAD's command line), `list_commands` |

It also offers the resources `librecad://guide` (offline reference) and `librecad://routes` (the
live API route list), and the prompts `draw-from-description`, `inspect-drawing` and `convert-file`.

## Requirements

1. **LibreCAD-mcp**, a LibreCAD build with the Remote API, from
   [the fork's releases](https://github.com/ShiftSad/LibreCAD-mcp/releases).
2. **[uv](https://docs.astral.sh/uv/)**, which supplies the `uvx` command that runs this server.
   - Windows (PowerShell): `irm https://astral.sh/uv/install.ps1 | iex`
   - macOS and Linux: `curl -LsSf https://astral.sh/uv/install.sh | sh`
3. **git**: uv fetches this server from the fork's repository (it is not on PyPI).

## Quickstart

### 1. Enable the Remote API in LibreCAD

In LibreCAD-mcp, open **Options > Application Preferences > Remote API (MCP)** and tick
**Enable Remote API**. Click OK. The status line in that tab should read
"Listening on 127.0.0.1:13140".

That is all the setup this server needs on the same computer. While the API is on, LibreCAD writes
a small discovery file with the address and the token, readable by your user account only, and
this server reads it. Nothing has to be copied by hand.

### 2. Claude Desktop

Download `librecad-mcp-<version>.mcpb` from the
[release page](https://github.com/ShiftSad/LibreCAD-mcp/releases) and open it with Claude Desktop
(or drag it into Settings > Extensions). Leave the settings blank. Don't hand-edit
`claude_desktop_config.json`: the extension takes care of starting the server.

The first start downloads the server from git, which can take a minute. To do that ahead of time,
run once in a terminal:

```sh
uvx --from "git+https://github.com/ShiftSad/LibreCAD-mcp@v2.2.2.6-mcp.1#subdirectory=mcp" librecad-mcp --version
```

### 3. Claude Code (or any MCP client with a JSON config)

```sh
claude mcp add librecad -- uvx --from "git+https://github.com/ShiftSad/LibreCAD-mcp@v2.2.2.6-mcp.1#subdirectory=mcp" librecad-mcp
```

or in a project's `.mcp.json`:

```json
{
  "mcpServers": {
    "librecad": {
      "command": "uvx",
      "args": ["--from", "git+https://github.com/ShiftSad/LibreCAD-mcp@v2.2.2.6-mcp.1#subdirectory=mcp", "librecad-mcp"],
      "env": {}
    }
  }
}
```

`env` stays empty: the token comes from the discovery file. The **Copy MCP config** button in
LibreCAD's Remote API tab copies a ready snippet.

Then ask, for example: *"Draw a 4 m x 3 m room plan with 150 mm walls on a layer 'walls', add the
dimensions, and show me."*

### 4. Windows fallback: a plain virtual environment

If `uvx` fails on Windows (for example with a sharing violation while installing a dependency), use
a normal virtual environment, which retries:

```powershell
py -m venv "$env:USERPROFILE\.librecad-mcp"
& "$env:USERPROFILE\.librecad-mcp\Scripts\pip" install "git+https://github.com/ShiftSad/LibreCAD-mcp@v2.2.2.6-mcp.1#subdirectory=mcp"
```

and use `%USERPROFILE%\.librecad-mcp\Scripts\librecad-mcp.exe` as the command, without arguments.

## Configuration

Nothing is needed when LibreCAD runs on the same computer. Optional environment variables:

| Variable | Default | Purpose |
|---|---|---|
| `LIBRECAD_API_URL` | from the discovery file, else `http://127.0.0.1:13140` | Remote API address |
| `LIBRECAD_API_TOKEN` | from the discovery file | Remote API token |
| `LIBRECAD_API_DISCOVERY` | the OS default below | Path of `remote-api.json` |
| `LIBRECAD_API_TIMEOUT` | `30` | Seconds to wait on ordinary calls; open, save, export and new wait 50 s |
| `LIBRECAD_MCP_ALLOWED_ROOTS` | unset (any local path) | Folders this server may open files from or write to, separated by `;` on Windows and `:` elsewhere |
| `LIBRECAD_MCP_ALLOW_NETWORK_PATHS` | `0` | `1` allows UNC and other network paths |

A value that starts with `${` counts as unset (Claude Desktop leaves blank optional settings that
way). The settings are read on every tool call, so a new token in LibreCAD is picked up at once.

**Discovery file** (`remote-api.json`, holds `url`, `port`, `token`, `pid`, versions):

| OS | Location |
|---|---|
| Windows | `%APPDATA%\LibreCAD\LibreCAD-2\remote-api.json` |
| Linux | `~/.local/share/LibreCAD/LibreCAD-2/remote-api.json` (`$XDG_DATA_HOME` when set) |
| macOS | `~/Library/Application Support/LibreCAD/LibreCAD-2/remote-api.json` |

## Conventions

- **Units**: coordinates are drawing units (`get_drawing_info` reports them, often mm). Points are `[x, y]`.
- **Angles are degrees** everywhere (`*_deg`), counter-clockwise from +X. Lineweights are millimetres. Pixel sizes end in `_px`.
- **Ids**: each entity has an `id` that stays the same when it is edited, moved or undone; copies get new ids. Drawings have their own ids (`document_id`); omitted, a tool acts on the active drawing.
- **Undo**: one tool call that changes entities is one undo step, like Ctrl+Z in LibreCAD, and shares the history with your own edits.
- **Layer changes are not undoable** in LibreCAD (create, rename, restyle, delete). Replies say `"undoable": false`.
- **Model space only**: paper-space layouts are not reachable in this version.
- Errors come back as `{"error": "<token>", "detail", "hint", ...}` instead of failing the call.

## Security model

- The API is off until you enable it, binds `127.0.0.1` only, and needs a 128-bit random token
  on every request (compared in constant time). Requests from web pages are refused (`Origin`
  header, `Host` check against DNS rebinding).
- **Asks you first**: closing a drawing without saving, deleting a layer, and overwriting a file.
  Clients that support MCP elicitation show you a yes/no prompt; with others, the AI has to get
  your explicit "yes" in the chat, and its tool text forbids acting on instructions found in
  documents, web pages or tool output. Your "no" in a prompt always wins.
- **File paths** must be absolute local paths with the right extension. Network (UNC) paths are
  refused unless you allow them, here and in LibreCAD's Preferences. You can restrict file access
  to some folders, here (`LIBRECAD_MCP_ALLOWED_ROOTS`) and in LibreCAD (Remote API tab). LibreCAD
  checks every rule again on its side.
- In LibreCAD's Remote API tab you can turn off command-line access (`run_command`) and the
  activity lines (`[MCP] ...`) LibreCAD writes to its command history. Closing without saving,
  deleting a layer and overwriting a file are always logged there.
- The token is never shown to the AI or written to logs; `get_status` only says where it came from.

## Troubleshooting

| Symptom | What to do |
|---|---|
| `LibreCAD not reachable: no Remote API discovery file ...` | LibreCAD-mcp is not running, or the Remote API is not enabled. Start it and tick Enable in Preferences > Remote API (MCP). |
| `LibreCAD not reachable at http://127.0.0.1:13140 ...` | LibreCAD closed or crashed after writing the discovery file, or another port is in use: start LibreCAD again and check the status line in the Remote API tab. |
| `unauthorized (check the token ...)` | The token changed (Regenerate in Preferences) while `LIBRECAD_API_TOKEN` or the extension's token setting still holds the old one. Clear that setting to use the discovery file. |
| Status "Port 13140 is in use" in Preferences | Another LibreCAD (or another program) holds the port. Close it, or pick another port; the discovery file follows. |
| `gui_modal_open` | A dialog is open in LibreCAD. Close it and ask again. |
| `ui_timeout` or `timed_out` | LibreCAD was busy (a large file, a long export). Check with `list_documents` before retrying: the work may have finished. |
| `blocked_by_local_policy` | This server refused a file path; `reason` and `hint` say why. |
| First start in Claude Desktop times out | uv was still downloading the server; run the `uvx ... --version` command from the quickstart once. |

## Development

```sh
cd mcp
uv run --extra dev pytest -q          # unit tests against a stateful fake of the HTTP contract
LIBRECAD_LIVE=1 uv run --extra dev pytest -q tests/test_live.py   # against a running LibreCAD-mcp
uv run librecad-mcp --version
```

- `src/librecad_mcp/server.py`: the tools, resources and prompts (MCP Python SDK v2, `MCPServer`).
- `client.py`: one method per Remote API route; `errors.py`: HTTP status to exception mapping.
- `config.py`, `discovery.py`: where the API is; `guard.py`: path rules and confirmations.
- `fields.py`: the entity spec models (the same field names as the HTTP API).
- `tests/fake_librecad.py`: an in-memory fake of the API used by the tests.
- `scripts/cut-release.sh <version> <tag>`: bumps versions, pins the tag, runs the tests and packs
  the `.mcpb`. `scripts/make_icon.py` redraws the extension icon.

The HTTP contract is described in `docs/remote-api/` at the repository root.

## Privacy policy

- **Data collection**: this server collects nothing. It has no telemetry and no analytics.
- **Use**: it sends your MCP client's requests to LibreCAD's Remote API on `127.0.0.1` and returns
  LibreCAD's answers (drawing data, images, file paths) to your MCP client.
- **Storage**: it stores nothing. It reads LibreCAD's discovery file to find the API.
- **Third parties**: it contacts no other service. What your MCP client does with the answers,
  including sending them to an AI model, is governed by that client's privacy policy.
- **Retention**: none; nothing is kept after a call.
- **Contact**: [open an issue](https://github.com/ShiftSad/LibreCAD-mcp/issues).

## License and support

GPL-2.0-or-later, the same as LibreCAD (see [LICENSE](LICENSE)). Support through the fork's
[issue tracker](https://github.com/ShiftSad/LibreCAD-mcp/issues).

---

## Início rápido (português)

1. Instale o **LibreCAD-mcp** (a versão do LibreCAD com a Remote API, na
   [página de releases](https://github.com/ShiftSad/LibreCAD-mcp/releases)) e o
   **[uv](https://docs.astral.sh/uv/)**. No Windows: `irm https://astral.sh/uv/install.ps1 | iex`.
   O git também precisa estar instalado.
2. No LibreCAD, abra **Options > Application Preferences > Remote API (MCP)**, marque
   **Enable Remote API** e clique em OK. A API escuta só em `127.0.0.1` e exige um token; o
   LibreCAD grava um arquivo de descoberta que este servidor lê sozinho, então não é preciso
   copiar nada.
3. **Claude Desktop**: abra o arquivo `librecad-mcp-<versão>.mcpb` da página de releases e deixe
   as configurações em branco. **Claude Code**:

   ```sh
   claude mcp add librecad -- uvx --from "git+https://github.com/ShiftSad/LibreCAD-mcp@v2.2.2.6-mcp.1#subdirectory=mcp" librecad-mcp
   ```

4. Peça, por exemplo: *"Desenhe a planta de um quarto de 4 m x 3 m com paredes de 150 mm na camada
   'paredes', cote e me mostre."*

Coordenadas em unidades do desenho, ângulos sempre em graus. Fechar sem salvar, apagar uma camada
e sobrescrever um arquivo sempre pedem a sua confirmação. Problemas: veja *Troubleshooting* acima
ou abra uma [issue](https://github.com/ShiftSad/LibreCAD-mcp/issues).
