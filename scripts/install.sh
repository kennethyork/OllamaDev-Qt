#!/usr/bin/env bash
# Build OllamaDev (CLI + desktop) and install it into ~/.local.
#
#   scripts/install.sh              # both binaries
#   ODV_BUILD_ADE=OFF scripts/install.sh   # CLI only (headless box, no Qt Widgets)
#   PREFIX=/usr/local sudo -E scripts/install.sh
set -euo pipefail
export LC_ALL=C
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

PREFIX="${PREFIX:-$HOME/.local}"
BUILD="${BUILD_DIR:-$ROOT/build}"
ADE="${ODV_BUILD_ADE:-ON}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

command -v cmake >/dev/null || { echo "✗ cmake is required" >&2; exit 1; }
command -v g++   >/dev/null || { echo "✗ g++ is required" >&2; exit 1; }

echo "▸ configuring (prefix: $PREFIX, ade: $ADE)…"
cmake -S "$ROOT" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DODV_BUILD_ADE="$ADE" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null

echo "▸ building…"
cmake --build "$BUILD" -j"$JOBS"

echo "▸ tests…"
ctest --test-dir "$BUILD" --output-on-failure

echo "▸ installing…"
cmake --install "$BUILD" >/dev/null

# The .desktop only becomes visible in the launcher once the database is
# refreshed; harmless if the tool is absent (headless, or a CLI-only install).
if [ "$ADE" = "ON" ] && command -v update-desktop-database >/dev/null; then
  update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true
fi

echo
echo "✓ installed to $PREFIX/bin"
"$PREFIX/bin/ollamadev" --version

# A previous install (or the PHP build, which also shipped a `ollamadev`) earlier
# on PATH would silently shadow what we just installed — the "I fixed it but the
# binary still misbehaves" trap. Say so rather than let it be discovered later.
ON_PATH="$(command -v ollamadev || true)"
if [ -n "$ON_PATH" ] && [ "$ON_PATH" != "$PREFIX/bin/ollamadev" ]; then
  echo
  echo "⚠ PATH resolves 'ollamadev' to $ON_PATH, NOT the copy just installed."
  echo "  That one wins in your shell. Remove it, or put $PREFIX/bin first in PATH."
elif [ -z "$ON_PATH" ]; then
  echo
  echo "⚠ $PREFIX/bin is not on your PATH. Add it:"
  echo "    export PATH=\"$PREFIX/bin:\$PATH\""
fi
