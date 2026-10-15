#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Prepare a LibreCAD MCP server release: bump the version markers, pin the server source to the
# release's git tag, run the tests and pack the Claude Desktop extension (.mcpb).
#
#   scripts/cut-release.sh 0.1.1 v2.2.2.6-mcp.2
#
# The tag is the one the LibreCAD-mcp release will get (v<LibreCAD version>-mcp.<N>). It must be
# the tag of this LibreCAD-mcp build: LC_VERSION (CMakeLists.txt) without its label, and
# LC_MCP_RELEASE / LC_MCP_TAG of librecad/src/remote_api/remote_api.{cmake,pri}, which LibreCAD's
# "Copy MCP config" uses. Bump LC_MCP_RELEASE (and LC_MCP_TAG in the .pri) first.
#
# Every place that names the server's git source (`LibreCAD-mcp@<tag>#subdirectory=mcp`) or a
# documentation link to a branch of the fork is pinned to the tag, never to a branch: a later push
# must not change what an installed extension runs. The .mcpb also carries the server itself
# (mcpb/server/python), so Claude Desktop needs neither git nor the published tag to run it.
#
# Nothing is published: the server is not on PyPI or the MCP registry. Committing, tagging, pushing
# the tag and attaching the .mcpb to a GitHub release of ShiftSad/LibreCAD-mcp stay manual steps,
# done after a review; scripts/check-release.sh <tag> then checks that the tag is published and
# runs the pinned source, before the release is announced.
set -euo pipefail
VER="${1:?version, e.g. 0.1.1}"
TAG="${2:?release tag, e.g. v2.2.2.6-mcp.2}"
if [[ ! "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.]+)?$ ]]; then
  echo "FAIL: the version must look like 0.1.1, got '$VER'" >&2; exit 1
fi
if [[ ! "$TAG" =~ ^v[0-9]+(\.[0-9]+)+-mcp\.[0-9]+$ ]]; then
  echo "FAIL: the tag must look like v2.2.2.6-mcp.2, got '$TAG'" >&2; exit 1
fi
cd "$(dirname "$0")/.."   # mcp/
REPO=..

echo "== 1/5 the tag is this build's =="
lc_version=$(grep -o 'LC_VERSION=[0-9][0-9.]*' "$REPO/CMakeLists.txt" | head -n1 | cut -d= -f2)
cmake_release=$(grep -o '^set(LC_MCP_RELEASE [0-9]*)' "$REPO/librecad/src/remote_api/remote_api.cmake" | grep -o '[0-9]*')
pri_release=$(grep -o '^DEFINES += LC_MCP_RELEASE=[0-9]*' "$REPO/librecad/src/remote_api/remote_api.pri" | grep -o '[0-9]*$')
cmake_tag_version=$(grep -o '^set(LC_MCP_TAG v[0-9.]*-mcp' "$REPO/librecad/src/remote_api/remote_api.cmake" | grep -o 'v[0-9.]*[0-9]')
pri_tag=$(sed -n 's/^DEFINES += LC_MCP_TAG=\(v[0-9.]*-mcp\.[0-9]*\).*/\1/p' "$REPO/librecad/src/remote_api/remote_api.pri")
expected="v${lc_version}-mcp.${cmake_release}"
if [ -z "$lc_version" ] || [ -z "$cmake_release" ] || [ "$cmake_release" != "$pri_release" ] ||
   [ "$cmake_tag_version" != "v${lc_version}" ] || [ "$pri_tag" != "$expected" ]; then
  echo "FAIL: LC_VERSION ($lc_version), LC_MCP_RELEASE (cmake $cmake_release, pri $pri_release) and LC_MCP_TAG" \
       "(cmake ${cmake_tag_version}-mcp.\${LC_MCP_RELEASE}, pri $pri_tag) disagree" >&2; exit 1
fi
if [ "$TAG" != "$expected" ]; then
  echo "FAIL: this build is $expected (LC_VERSION $lc_version, LC_MCP_RELEASE $cmake_release), not $TAG;" \
       "bump LC_MCP_RELEASE (and LC_MCP_TAG in remote_api.pri) first" >&2; exit 1
fi
echo "   $TAG is LibreCAD $lc_version, LibreCAD-mcp build $cmake_release"

echo "== 2/5 bumping the version markers to $VER =="
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

echo "== 3/5 pinning the server source and the documentation links to $TAG =="
# Every file outside mcp/scripts and mcp/tests that names the git source.
# (Not librecad/: "Copy MCP config" builds its line from LC_MCP_TAG, checked in step 1.)
mapfile -t PINNED < <(git -C "$REPO" grep -l 'LibreCAD-mcp@[^#"]*#subdirectory=mcp' -- . \
                        ':!mcp/scripts' ':!mcp/tests' ':!librecad' | sed "s|^|$REPO/|")
for must in mcp/mcpb/server/index.js mcp/server.json mcp/README.md docs/remote-api/README.md docs/remote-api/DESIGN.md; do
  if ! printf '%s\n' "${PINNED[@]}" | grep -qx "$REPO/$must"; then
    echo "FAIL: $must no longer names the server source (LibreCAD-mcp@<tag>#subdirectory=mcp)" >&2; exit 1
  fi
done
sed -i "s|LibreCAD-mcp@[^#\"]*#subdirectory=mcp|LibreCAD-mcp@$TAG#subdirectory=mcp|g" "${PINNED[@]}"
# documentation links: .../LibreCAD-mcp/(tree|blob)/<branch>/mcp... -> the tag
LINKED=(pyproject.toml server.json mcpb/manifest.json)
sed -i -E "s#(github\.com/ShiftSad/LibreCAD-mcp/(tree|blob)/)[^/\"]+/#\1$TAG/#g" "${LINKED[@]}"
mapfile -t refs < <(grep -ho 'LibreCAD-mcp@[^#"]*#subdirectory=mcp' "${PINNED[@]}" | sort -u)
mapfile -t links < <(grep -hoE 'LibreCAD-mcp/(tree|blob)/[^/"]+/' "${LINKED[@]}" | sed -E 's#.*/(tree|blob)/##' | sort -u)
if [ "${#refs[@]}" -ne 1 ] || [ "${refs[0]}" != "LibreCAD-mcp@$TAG#subdirectory=mcp" ] ||
   ! grep -q "LibreCAD-mcp@$TAG#subdirectory=mcp" mcpb/server/index.js ||
   { [ "${#links[@]}" -gt 0 ] && [ "${links[*]}" != "$TAG/" ]; }; then
  echo "FAIL: not everything is pinned to $TAG: ${refs[*]} ${links[*]}" >&2; exit 1
fi
echo "   ${#PINNED[@]} files run @$TAG; documentation links point at $TAG"

echo "== 4/5 tests =="
uv lock
uv run --extra dev pytest -q

echo "== 5/5 pack the Claude Desktop extension (with the server inside) =="
rm -rf mcpb/server/python
mkdir -p mcpb/server/python
cp pyproject.toml uv.lock README.md LICENSE mcpb/server/python/
cp -R src mcpb/server/python/src
find mcpb/server/python -name '__pycache__' -type d -prune -exec rm -rf {} +
cp LICENSE mcpb/LICENSE
npx --yes @anthropic-ai/mcpb validate mcpb/manifest.json
npx --yes @anthropic-ai/mcpb pack mcpb "librecad-mcp-$VER.mcpb"
sha256sum "librecad-mcp-$VER.mcpb" > "librecad-mcp-$VER.mcpb.sha256"

if [ -z "$(git -C "$REPO" ls-remote --tags origin "refs/tags/$TAG" 2>/dev/null)" ]; then
  echo "NOTE: $TAG is not on origin yet. The .mcpb runs its own copy of the server, but every"
  echo "      'uvx --from git+...@$TAG' line in the docs fails until the tag is pushed: run"
  echo "      scripts/check-release.sh $TAG after pushing it, before announcing the release."
fi
echo "DONE: librecad-mcp-$VER.mcpb (+ .sha256). Review, commit, and tag that commit $TAG by hand."
