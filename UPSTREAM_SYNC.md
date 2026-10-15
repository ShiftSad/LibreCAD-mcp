# Keeping LibreCAD-mcp in sync with upstream LibreCAD

LibreCAD-mcp ([ShiftSad/LibreCAD-mcp](https://github.com/ShiftSad/LibreCAD-mcp)) is upstream
[LibreCAD](https://github.com/LibreCAD/LibreCAD) plus two things:

1. **DWG reading fixes** for AutoCAD 2018-2026 (AC1032) drawings, in the DWG/DXF filter.
2. **The Remote API and the MCP server**: a local HTTP API inside LibreCAD
   ([docs](docs/remote-api/README.md), [design](docs/remote-api/DESIGN.md)) and the `librecad-mcp`
   Python server in [`mcp/`](mcp/README.md).

This file explains how to move the fork to a new upstream LibreCAD, and the version and tag rules.
LibreCAD changes often, so the diff against upstream is kept small on purpose: new code lives in
its own folders, and changes to upstream files are short hooks marked `librecad-mcp:`.

| Fork-only path | What |
|---|---|
| `librecad/src/remote_api/` | the Remote API (core, entities, documents) and its C++ tests |
| `mcp/` | the Python MCP server, its tests and packaging |
| `tools/remote-api/` | the end-to-end test |
| `docs/remote-api/`, `UPSTREAM_SYNC.md` | documentation |
| `.github/workflows/mcp-tests.yml`, `CI/package-windows-portable.ps1` | CI |

## Current state

Update this table every time you sync or release.

| What | Value |
|---|---|
| Upstream base | `LibreCAD/LibreCAD` `master` at `41678d97` (2026-10-14) |
| `LC_VERSION` (upstream's, never changed here) | `2.2.2.6-alpha` (`CMakeLists.txt`, `librecad/src/src.pro`) |
| `LC_MCP_RELEASE` | `1` (`librecad/src/remote_api/remote_api.cmake` and `remote_api.pri`) |
| Python package | `librecad-mcp` `0.1.0` (`mcp/pyproject.toml`) |
| Fork commits that touch upstream code outside the hooks | `df7bcd45` (AC1032 labelled "AutoCAD 2018-2026"), `931aba30` (paper-space content kept out of model space on import) |
| Last release tag | none yet; the first is `v2.2.2.6-mcp.1` |

Remotes (one-time setup; `origin` is the fork):

```bash
git remote add upstream https://github.com/LibreCAD/LibreCAD.git
```

## 1. The hook map

Every line the fork adds or changes inside an upstream file carries the marker `librecad-mcp:`
(in C++, CMake, qmake, YAML and shell comments). List them all with:

```bash
git grep -n "librecad-mcp:" -- . ':!docs' ':!*.md' ':!librecad/src/remote_api' ':!mcp' ':!tools/remote-api'
```

The table is what that command finds once all parts of the Remote API are merged. When it and the
command disagree, the command is right: update the table.

| File | Where | Hook |
|---|---|---|
| `CMakeLists.txt` | after `set (SHARED_SOURCES ...)` | `include(.../remote_api/remote_api.cmake)`, then append `LC_REMOTE_API_SOURCES`/`INCLUDES` |
| `CMakeLists.txt` | after `librecad_lib` and after `librecad` | `target_compile_definitions(... PRIVATE ${LC_REMOTE_API_DEFINES})` (`LC_MCP_RELEASE` and the `LC_REMOTE_API_HAS_*` macros) |
| `CMakeLists.txt` | inside `if (BUILD_TESTS)` | the `librecad_remote_api_tests` target |
| `librecad/src/src.pro` | next to the other `include()`s | `include($$PWD/remote_api/remote_api.pri)` (qmake is still built by CodeQL) |
| `.gitignore` | after the core-dump patterns | re-include `librecad/src/remote_api/core/`, which `core` and `core.*` would hide |
| `librecad/src/ui/main/qc_applicationwindow.h` | class body | forward declaration, `m_remoteApi` next to `m_releaseChecker`, `remoteApi()`, `friend class LC_RemoteApiAppAccess` |
| `librecad/src/ui/main/qc_applicationwindow.cpp` | includes, `~QC_ApplicationWindow()`, `initCompleted()`, `slotOptionsGeneral()`, `tryCloseAllBeforeExist()`, `autoSaveCurrentDrawing()` | include the controller; stop it first in the destructor; create it and `startIfEnabled()`; `applySettings()` on OK; `stop()` once exit is confirmed; skip autosave while an API task runs |
| `librecad/src/ui/dialogs/settings/options_general/qg_dlgoptionsgeneral.{h,cpp}` | constructor, `ok()` | the "Remote API (MCP)" tab and its `save()` |
| `librecad/src/main/main.cpp` | before `RS_Settings::init`, window title | `LIBRECAD_SETTINGS_DIR` (INI settings in a folder, for tests); `" MCP"` in the title |
| `librecad/src/ui/main/qc_mdiwindow.{h,cpp}` | `loadDocument` | a defaulted `onImportError` callback, passed on |
| `librecad/src/ui/main/persistence/lc_documentsstorage.{h,cpp}` | `loadDocument`, `loadGraphic` | the same callback, passed to `RS_FileIO::fileImport` (no message boxes on import errors) |
| `librecad/src/lib/generators/image/lc_imageexporter.{h,cpp}` | `ExportOptions`, `prepareViewport` | a world `window` (null means auto) and `renderToImage` into a `QImage` |
| `librecad/src/ui/main/release_check/lc_releasechecker.cpp` | `checkForNewVersion`, `parseTagInfo`, `isBefore`, `isSameVersion`, `getOwnReleaseInfo` | the fork's releases URL; `-mcp.N` tags parsed and ordered by N first; this build's own N from `LC_MCP_RELEASE` |
| `librecad/src/ui/dialogs/main/lc_dlgabout.cpp` | constructor | "LibreCAD-mcp build: N" line; "The Code" links to the fork, plus an "Upstream LibreCAD" link |
| `librecad/src/ui/main/init/lc_menufactory_main.cpp` | `createHelpMenu` | Submit Error, Request Feature and Releases Page go to the fork (forum and chat stay upstream's); "Dev Snapshot Release" is left out of MCP builds |
| `.github/workflows/build-all.yml` | triggers, every upload step, the Windows jobs, `UpdateAssets` | `v*-mcp.*` tags trigger it; artifacts `LibreCAD-mcp_*`; Windows portable zip and `SHA256SUMS-win-<arch>.txt`; `UpdateAssets` (which only runs in the LibreCAD organisation) follows the new artifact names |
| `README.md` | top | the fork banner (the `> [!IMPORTANT]` block; Markdown has no marker) |

**Code copied from upstream** has no marker but must follow upstream changes. After a sync, diff
these upstream places and port what changed:

| Fork code | Copied from |
|---|---|
| `LC_RemoteApiAppAccess::openDocument` (`remote_api/documents/lc_remoteapiappaccess.cpp`) | `QC_ApplicationWindow::openFile()` in `qc_applicationwindow.cpp`, minus its `QMessageBox` calls |
| `LC_RemoteApiRenderer::exportPdf` (`remote_api/documents/lc_remoteapirenderer.cpp`) | the PDF branch of `LC_Printing::print()` in `librecad/src/lib/printing/lc_printing.cpp` |
| the format table in `remote_api/documents/lc_remoteapiformats.cpp` | `RS2::FormatType` in `librecad/src/lib/engine/rs.h` and the DWG version mapping in `librecad/src/main/console_command_utils.cpp` (not linked into `librecad_lib`, so it is copied) |

## 2. Moving to a new upstream LibreCAD

Commands are for Git Bash or any POSIX shell. `OLD` is the base in the table above.

### 2.1 Look before you move

```bash
git fetch upstream
OLD=41678d97
NEW=upstream/master
git log --oneline $OLD..$NEW | wc -l

# Upstream files the fork touches, and how much upstream changed each of them:
TOUCHED=$(git diff --name-only $OLD master -- . ':!librecad/src/remote_api' ':!mcp' \
    ':!tools/remote-api' ':!docs/remote-api' ':!UPSTREAM_SYNC.md' \
    ':!CI/package-windows-portable.ps1' ':!.github/workflows/mcp-tests.yml')
git diff --stat $OLD $NEW -- $TOUCHED
```

Large changes in `qc_applicationwindow.cpp`, `qg_dlgoptionsgeneral.cpp`, `main.cpp`,
`build-all.yml` or the DWG filter (`librecad/src/lib/filters/rs_filterdxfrw.cpp`,
`libraries/libdxfrw/`) mean manual work ahead.

### 2.2 Rebase on a new branch

```bash
git switch -c sync-$(date +%Y%m%d) master
git rebase --onto $NEW $OLD
```

`--onto $NEW $OLD` replays exactly the fork's commits (everything after `$OLD`). Never rewrite
`master` in place: replacing it needs a force-push, which must be agreed with the repository
owner first. When the fork has outside contributors, merging `upstream/master` into `master`
instead of rebasing avoids the force-push; the rest of this section applies the same way.

### 2.3 Rules while resolving conflicts

- Take upstream's new code, then put the hook back on top. Keep hooks short and keep the
  `librecad-mcp:` marker on them.
- Never delete upstream behaviour. When the fork must replace it, leave upstream's code
  unreachable (`#ifndef LC_MCP_RELEASE`, or a comment with the upstream value) instead of deleting it.
- Never append anything to `LC_VERSION` or `LC_PRERELEASE`: upstream uses them in file metadata,
  the About box and the settings migration. The fork's own number is `LC_MCP_RELEASE`.
- The DWG fix commits (`df7bcd45`, `931aba30`, and later ones): if upstream has fixed the same
  thing, git stops on an empty or conflicting commit. Prefer upstream's version, `git rebase --skip`
  ours, and remove it from the table above. The MCP work itself never touches
  `librecad/src/lib/filters/` or `libraries/libdxfrw/`.

### 2.4 Check what changed under the API

Compiling catches changed signatures. It does not catch changed behaviour, such as a new modal
dialog on a path the API uses. Look at what the Remote API calls, and for new dialogs:

```bash
git diff $OLD $NEW -- librecad/src/ui/main/qc_applicationwindow.cpp librecad/src/ui/main/qc_mdiwindow.cpp \
    librecad/src/ui/main/persistence librecad/src/lib/fileio librecad/src/lib/printing \
    librecad/src/lib/generators/image librecad/src/ui/dock_widgets/command_line \
    librecad/src/ui/qg_actionhandler.cpp librecad/src/lib/engine/undo librecad/src/lib/engine/document \
  | grep -nE '^\+.*(QMessageBox|QFileDialog|QInputDialog|QDialog|exec\(\))'

# New file formats or DWG versions: add a row to remote_api/documents/lc_remoteapiformats.cpp
git diff $OLD $NEW -- librecad/src/lib/engine/rs.h | grep -n Format

# New typed commands (a command that opens a dialog is closed by the modal guard and reported
# in skipped_dialogs; one that opens a file or print dialog must not be reachable from /command)
git diff $OLD $NEW -- librecad/src/cmd/lc_commandItems.h
```

Then compare the copied code (table in section 1) with its upstream source.

### 2.5 Build and test

```bash
# C++: CMake build with tests, then the Remote API tests and upstream's fast tests
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build --target librecad librecad_remote_api_tests librecad_fast_tests
QT_QPA_PLATFORM=offscreen ctest --test-dir build -R "librecad_remote_api_tests|librecad_fast_tests" --output-on-failure

# qmake still has to build (CodeQL uses it): every new C++ file is in a .pri as well as a .cmake
qmake6 -r librecad.pro && make -j

# Python MCP server
(cd mcp && uv run --locked --extra dev pytest -q)

# End to end, against the new build, also through the MCP server
python3 tools/remote-api/e2e_smoke.py --exe build/librecad --via-mcp
```

On Windows, run the e2e test with `--qt-bin <Qt>/bin` for a development build (see
[`tools/remote-api/README.md`](tools/remote-api/README.md)).

### 2.6 Finish

1. Update the "Current state" table (base, fork commits) and, if the hooks moved, the hook map.
2. Push the `sync-*` branch and open a pull request into `master` of the fork, so CI runs.
3. Move `master` only as agreed in 2.2.

## 3. Versions, tags and releases

### 3.1 Rules

- **`LC_VERSION` is upstream's and is never changed** by the fork (see 2.3).
- **`LC_MCP_RELEASE` is the fork's build number N.** It is set in two places that must stay
  equal: `set(LC_MCP_RELEASE N)` in `librecad/src/remote_api/remote_api.cmake` and
  `DEFINES += LC_MCP_RELEASE=N` in `librecad/src/remote_api/remote_api.pri`. N is **global and
  only goes up**, also when the LibreCAD version underneath changes. `/status` reports it as
  `mcp_build`, and the About box shows it.
- **Release tags** are `v<LC_VERSION without its label>-mcp.<N>`: `LC_VERSION` `2.2.2.6-alpha`
  with N = 1 gives **`v2.2.2.6-mcp.1`**. One tag per N; a tag is never moved or reused.
- **The in-app update check** (`lc_releasechecker.cpp`) reads the fork's GitHub releases, parses
  `-mcp.N` and orders by N first, so a build offers every release with a higher N, and never a tag
  without the suffix. A release whose tag breaks the pattern is invisible to it.
- **The Python package** (`mcp/pyproject.toml`, `mcp/server.json`, `mcp/mcpb/manifest.json`) has
  its own semantic version. `mcp/scripts/cut-release.sh <version> <tag>` bumps all three and pins
  the tag in the `uvx --from git+...@<tag>` lines, the `.mcpb` launcher and the README.

### 3.2 Making a release

1. In one commit ("Release LibreCAD-mcp N"): bump `LC_MCP_RELEASE` in both files, and run
   `mcp/scripts/cut-release.sh <python version> v<version>-mcp.<N>`.
2. Everything in 2.5 passes.
3. Only after the repository owner confirms: tag and push the tag.

   ```bash
   git tag -a v2.2.2.6-mcp.1 -m "LibreCAD-mcp 1 (LibreCAD 2.2.2.6-alpha)"
   git push origin v2.2.2.6-mcp.1
   ```

4. The tag starts `.github/workflows/build-all.yml`. Its artifacts are `LibreCAD-mcp_linux-x86_64`,
   `LibreCAD-mcp_linux-aarch64`, `LibreCAD-mcp_snap`, `LibreCAD-mcp_macos-universal`,
   `LibreCAD-mcp_win-x64` and `LibreCAD-mcp_win-arm64`; each Windows one holds the installer, the
   portable zip and `SHA256SUMS-win-<arch>.txt`. Artifacts expire after 2 days, so download them
   promptly (`gh run download <run id>`).
5. Create the GitHub release on the fork with those files and the `.mcpb` from `cut-release.sh`
   (`gh release create v2.2.2.6-mcp.1 --repo ShiftSad/LibreCAD-mcp ...`). Mark it as a pre-release
   while `LC_VERSION` carries a label such as `alpha`.

`build-all.yml`'s `UpdateAssets` job publishes upstream's `continuous` release and only runs in the
LibreCAD organisation; it never runs in the fork, so releases there are always made by hand as
above.
