#!/usr/bin/env bash
# Build and install OllamaDev into ~/.local — the CLI, the desktop app, its menu
# entry and its icon.
#
# This exists because of a genuinely nasty failure mode: the binary on your PATH
# and the binary you just built are two different files. You change something, you
# build it, you run `ollamadev` — and you are running last week's copy, wondering
# why your change did nothing. Worse in the GUI, where a stale desktop entry can
# quietly launch a completely different application.
#
# So: one command, and everything on the machine is the thing you just built.
set -euo pipefail

cd "$(dirname "$0")"
PREFIX="${1:-$HOME/.local}"

echo "building…"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc)"

echo "testing…"
./build/tests/odv-tests | tail -1

echo "installing to $PREFIX…"
cmake --install build --prefix "$PREFIX" >/dev/null

# A .desktop file that is not indexed does not appear in the menu.
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true
fi

echo
echo "installed:"
echo "  $PREFIX/bin/ollamadev        $("$PREFIX/bin/ollamadev" --version)"
echo "  $PREFIX/bin/ollamadev-ade    (menu: OllamaDev ADE)"

case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) echo
       echo "WARNING: $PREFIX/bin is not on your PATH, so the shell will keep using"
       echo "         whatever older ollamadev it finds first. Add this to ~/.bashrc:"
       echo "           export PATH=\"$PREFIX/bin:\$PATH\"" ;;
esac
