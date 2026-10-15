#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# The release gate: run it after pushing a release tag, before the GitHub release is announced.
#
#   scripts/check-release.sh v2.2.2.6-mcp.2
#
# Fails unless the tag is on origin, the tagged tree has the server (mcp/pyproject.toml), every
# git source pinned in that tree names the same tag, and uvx can start the server from it. Every
# documented install line (`uvx --from "git+...@<tag>#subdirectory=mcp"`, Preferences > Copy MCP
# config of a release build) depends on exactly that.
set -euo pipefail
TAG="${1:?release tag, e.g. v2.2.2.6-mcp.2}"
REMOTE="${REMOTE:-origin}"
cd "$(dirname "$0")/../.."   # the repository root

echo "== 1/3 $TAG is published on $REMOTE =="
if [ -z "$(git ls-remote --tags "$REMOTE" "refs/tags/$TAG")" ]; then
  echo "FAIL: $REMOTE has no tag $TAG: push it first (git push $REMOTE $TAG)" >&2; exit 1
fi
git fetch --quiet "$REMOTE" "refs/tags/$TAG:refs/tags/$TAG"
if ! git cat-file -e "$TAG:mcp/pyproject.toml" 2>/dev/null; then
  echo "FAIL: the tagged tree has no mcp/pyproject.toml" >&2; exit 1
fi

echo "== 2/3 everything in it is pinned to $TAG =="
mapfile -t refs < <(git grep -ho 'LibreCAD-mcp@[^#"]*#subdirectory=mcp' "$TAG" -- . ':!mcp/scripts' ':!mcp/tests' ':!librecad' \
                    | sed 's/^[^:]*://' | sort -u)
if [ "${#refs[@]}" -ne 1 ] || [ "${refs[0]}" != "LibreCAD-mcp@$TAG#subdirectory=mcp" ]; then
  echo "FAIL: the tagged tree pins ${refs[*]:-nothing}" >&2; exit 1
fi
if ! git show "$TAG:librecad/src/remote_api/remote_api.pri" | grep -q "LC_MCP_TAG=$TAG"; then
  echo "FAIL: LC_MCP_TAG of the tagged tree is not $TAG" >&2; exit 1
fi

echo "== 3/3 uvx starts the server from git =="
uvx --refresh --from "git+https://github.com/ShiftSad/LibreCAD-mcp@$TAG#subdirectory=mcp" librecad-mcp --version

echo "DONE: $TAG is published and installable."
