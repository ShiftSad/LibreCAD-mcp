#!/usr/bin/env node
// SPDX-License-Identifier: GPL-2.0-or-later
// Claude Desktop extension launcher for the LibreCAD MCP server.
//
// The extension host runs this with Claude Desktop's bundled Node. The actual MCP server is the
// librecad-mcp Python package in the mcp/ folder of the LibreCAD-mcp repository, run via uvx
// straight from git (it is not on PyPI). uv needs git on the PATH to fetch it. GUI apps on macOS
// and Windows get a stripped PATH that misses user-level install dirs, so uvx is resolved
// against the places uv actually installs to before PATH.
"use strict";

const { spawn } = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

const WIN = process.platform === "win32";
const EXE = WIN ? "uvx.exe" : "uvx";
// Where uvx gets the server: the mcp/ folder at the release tag this extension was packed for
// (scripts/cut-release.sh writes it). Never a branch: a push to it would change what every
// installed extension runs, untested and maybe newer than the user's LibreCAD-mcp build.
const SOURCE = "git+https://github.com/ShiftSad/LibreCAD-mcp@v2.2.2.6-mcp.1#subdirectory=mcp";

function candidateDirs() {
  const home = os.homedir();
  const dirs = [
    path.join(home, ".local", "bin"), // uv installer default (all platforms)
    path.join(home, ".cargo", "bin"), // legacy uv installs
  ];
  if (WIN) {
    if (process.env.LOCALAPPDATA) {
      dirs.push(path.join(process.env.LOCALAPPDATA, "Programs", "uv"));
      dirs.push(path.join(process.env.LOCALAPPDATA, "Microsoft", "WinGet", "Links"));
    }
  } else {
    dirs.push("/opt/homebrew/bin", "/usr/local/bin", "/usr/bin");
  }
  for (const d of (process.env.PATH || "").split(path.delimiter)) {
    if (d) dirs.push(d);
  }
  return dirs;
}

function findUvx() {
  for (const dir of candidateDirs()) {
    const p = path.join(dir, EXE);
    try {
      if (fs.statSync(p).isFile()) return p;
    } catch {}
  }
  return null;
}

const uvx = findUvx();
if (!uvx) {
  process.stderr.write(
    "librecad-mcp: uv is not installed (uvx not found).\n" +
      "Install it, then reload this extension:\n" +
      (WIN
        ? "  PowerShell:  irm https://astral.sh/uv/install.ps1 | iex\n"
        : "  Terminal:  curl -LsSf https://astral.sh/uv/install.sh | sh\n") +
      "Searched: " + candidateDirs().join(path.delimiter) + "\n"
  );
  process.exit(1);
}

// Hand the child a usable PATH even when the extension host spawned us with a stripped one:
// uv's own dir plus the system dirs its launcher shims rely on.
const pathParts = [process.env.PATH, path.dirname(uvx)];
if (!WIN) pathParts.push("/usr/bin", "/bin", "/usr/sbin", "/sbin");
// Pipe stdio through this process instead of inheriting it. With "inherit", libuv on Windows
// skips CREATE_NO_WINDOW, so uvx (a console app) would get a fresh, merely hidden console under
// the console-less Claude Desktop parent, and the Python server could start with sys.stdin unset.
// Pipes sidestep that: the child gets plain anonymous-pipe handles and no console at all.
const child = spawn(uvx, ["--from", SOURCE, "librecad-mcp"], {
  stdio: ["pipe", "pipe", "pipe"],
  windowsHide: true,
  env: { ...process.env, PATH: pathParts.filter(Boolean).join(path.delimiter) },
});
process.stdin.pipe(child.stdin);
child.stdout.pipe(process.stdout);
child.stderr.pipe(process.stderr);
// The MCP host signals shutdown by closing our stdin; forward that so the Python server sees
// EOF and exits instead of lingering.
process.stdin.on("end", () => { try { child.stdin.end(); } catch {} });

child.on("error", (err) => {
  process.stderr.write("librecad-mcp: failed to start via " + uvx + ": " + err.message + "\n");
  process.exit(1);
});
child.on("exit", (code, signal) => {
  process.exit(signal ? 1 : code === null ? 1 : code);
});
for (const sig of ["SIGINT", "SIGTERM"]) {
  process.on(sig, () => {
    try { child.kill(sig); } catch {}
  });
}
