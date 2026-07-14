#!/usr/bin/env bash
# One command to cut a release: bump the version, commit, tag, and push — CI then
# builds every artifact (deb, rpm, AppImage, tar.gz) and publishes the release.
#
#   scripts/release.sh v0.2.1
#
# The tag push is what triggers the release job in .github/workflows/ci.yml; you
# can also run that job by hand from the Actions tab (workflow_dispatch).
set -euo pipefail
cd "$(dirname "$0")/.."

VER="${1:-}"
case "$VER" in
  v[0-9]*.[0-9]*.[0-9]*) : ;;
  *) echo "usage: scripts/release.sh vX.Y.Z   (e.g. v0.2.1)" >&2; exit 2 ;;
esac
NUM="${VER#v}"

# Refuse to release a dirty or unpushed tree — a tag must point at real, shared
# history, or the published artifacts won't match anything anyone can check out.
[ -z "$(git status --porcelain)" ] || { echo "✗ working tree is dirty — commit first" >&2; exit 1; }
git rev-parse "$VER" >/dev/null 2>&1 && { echo "✗ tag $VER already exists" >&2; exit 1; }

echo "▸ bumping to $NUM"
sed -i "s/#define ODV_VERSION \"[0-9.]*\"/#define ODV_VERSION \"$NUM\"/" core/Version.h
sed -i "s/project(ollamadev-qt VERSION [0-9.]*/project(ollamadev-qt VERSION $NUM/" CMakeLists.txt

git add core/Version.h CMakeLists.txt
git commit -q -m "release $VER"
git tag -a "$VER" -m "OllamaDev-Qt $VER"

echo "▸ pushing main + $VER"
git push -q origin HEAD
git push -q origin "$VER"

echo "✓ tagged $VER and pushed. CI is building the release now:"
echo "  https://github.com/kennethyork/OllamaDev-Qt/actions"
