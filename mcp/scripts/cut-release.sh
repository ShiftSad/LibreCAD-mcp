#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Prepare a LibreCAD MCP server release: bump the version markers, pin the server source to the
# release's git tag, run the tests and pack the Claude Desktop extension (.mcpb).
#
#   scripts/cut-release.sh 0.1.1 v2.2.2.6-mcp.2
#
# The tag is the one the LibreCAD-mcp release will get (v<LibreCAD version>-mcp.<N>). The
# launcher (mcpb/server/index.js), server.json and the README run the server from git at that
# tag, never from a branch: a later push must not change what an installed extension runs.
#
# Nothing is published: the server is not on PyPI or the MCP registry; users run it from git, as
# the .mcpb launcher does. Committing, tagging and attaching the .mcpb to a GitHub release of
# ShiftSad/LibreCAD-mcp stay manual steps, done after a review.
set -euo pipefail
VER="${1:?version, e.g. 0.1.1}"
TAG="${2:?release tag, e.g. v2.2.2.6-mcp.2}"
if [[ ! "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.]+)?$ ]]; then
  echo "FAIL: the version must look like 0.1.1, got '$VER'" >&2; exit 1
fi
if [[ ! "$TAG" =~ ^v[0-9]+(\.[0-9]+)+-mcp\.[0-9]+$ ]]; then
  echo "FAIL: the tag must look like v2.2.2.6-mcp.2, got '$TAG'" >&2; exit 1
fi
cd "$(dirname "$0")/.."

echo "== 1/4 bumping the version markers to $VER =="
sed -i "s/^version = \".*\"/version = \"$VER\"/" pyproject.toml
sed -i "s/^\(  \"version\": \)\"[^\"]*\"/\1\"$VER\"/" server.json mcpb/manifest.json

# All must agree or the extension ships mismatched metadata. Anchored so fields like
# manifest_version don't leak in.
mapfile -t found < <({ grep -ho '^version = "[^"]*"' pyproject.toml; \
                       grep -ho '^  "version": "[^"]*"' server.json mcpb/manifest.json; } \
                     | grep -o '"[^"]*"$' | tr -d '"' | sort -u)
if [ "${#found[@]}" -ne 1 ] || [ "${found[0]}" != "$VER" ]; then
  echo "FAIL: version markers disagree: ${found[*]}" >&2; exit 1
fi
echo "   all markers at $VER"

echo "== 2/4 pinning the server source to $TAG =="
PINNED=(mcpb/server/index.js server.json README.md)
sed -i "s|LibreCAD-mcp@[^#\"]*#subdirectory=mcp|LibreCAD-mcp@$TAG#subdirectory=mcp|g" "${PINNED[@]}"
mapfile -t refs < <(grep -ho 'LibreCAD-mcp@[^#"]*#subdirectory=mcp' "${PINNED[@]}" | sort -u)
if [ "${#refs[@]}" -ne 1 ] || [ "${refs[0]}" != "LibreCAD-mcp@$TAG#subdirectory=mcp" ] ||
   ! grep -q "LibreCAD-mcp@$TAG#subdirectory=mcp" mcpb/server/index.js; then
  echo "FAIL: the server source is not pinned to $TAG everywhere: ${refs[*]}" >&2; exit 1
fi
echo "   launcher, server.json and README run @$TAG"

echo "== 3/4 tests =="
uv lock
uv run --extra dev pytest -q

echo "== 4/4 pack the Claude Desktop extension =="
npx --yes @anthropic-ai/mcpb validate mcpb/manifest.json
npx --yes @anthropic-ai/mcpb pack mcpb "librecad-mcp-$VER.mcpb"
sha256sum "librecad-mcp-$VER.mcpb" > "librecad-mcp-$VER.mcpb.sha256"

echo "DONE: librecad-mcp-$VER.mcpb (+ .sha256). Review, commit, and tag that commit $TAG by hand."
