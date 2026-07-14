#!/usr/bin/env bash
# Build a self-contained Linux AppImage carrying BOTH binaries — the desktop app
# (ollamadev-ade) and the agent CLI (ollamadev) — plus the Qt6 runtime they
# actually link against.
#
# GLIBC FLOOR: an AppImage cannot bundle glibc (the bundled loader would have to
# match the host kernel/NSS), so the oldest system it runs on is the one it was
# BUILT on. Ubuntu 22.04 / Mint 21 (glibc 2.35) is the floor we ship, which is
# why .github/workflows/ci.yml builds the release AppImage on ubuntu-22.04.
# Building on a newer distro produces a working AppImage that simply refuses to
# start on older ones; the script prints the floor it actually achieved so this
# is never a silent surprise.
#
#   scripts/build-appimage.sh
set -euo pipefail
export LC_ALL=C
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
BUILD="$ROOT/.build/appimage"
APPDIR="$ROOT/.build/AppDir"
DIST="$ROOT/dist"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

case "$(uname -m)" in
  x86_64)        AIARCH=x86_64 ;;
  aarch64|arm64) AIARCH=aarch64 ;;
  *) echo "✗ unsupported arch: $(uname -m)" >&2; exit 1 ;;
esac

command -v cmake >/dev/null || { echo "✗ cmake is required" >&2; exit 1; }
QMAKE="$(command -v qmake6 || command -v qmake || true)"
[ -n "$QMAKE" ] || { echo "✗ qmake6 not found — install qt6-base-dev" >&2; exit 1; }

echo "▸ arch: $AIARCH · Qt: $("$QMAKE" -query QT_VERSION)"

# 1. Build both binaries fresh, so the AppImage can never carry a stale CLI.
echo "▸ building…"
# Force the reader browser: an AppImage is self-contained, so it cannot "borrow"
# the host's WebEngine — it would have to bundle ~90 MB of Chromium. The deb/rpm
# take the full browser as a system dependency instead; here we stay lean.
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DODV_BUILD_ADE=ON \
      -DODV_WEBENGINE=OFF >/dev/null
cmake --build "$BUILD" -j"$JOBS" >/dev/null

# 2. Lay out the AppDir. This clears $ROOT/.build/AppDir — a staging directory
#    this script owns and recreates every run, NOT a shared/runtime one. Nothing
#    here ever touches ~/.ollamadev, where a wipe would trash a running instance.
echo "▸ assembling AppDir…"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/plugins"
cp "$BUILD/cli/ollamadev"     "$APPDIR/usr/bin/"
cp "$BUILD/ade/ollamadev-ade" "$APPDIR/usr/bin/"

# 3. Qt plugins. These are dlopen()ed, so ldd on the binaries CANNOT see them —
#    an AppImage that skips this step builds fine and then dies at startup with
#    "could not load the Qt platform plugin xcb". Ask Qt where its plugins live
#    rather than hardcoding a distro path.
QT_PLUGINS="$("$QMAKE" -query QT_INSTALL_PLUGINS)"
for group in platforms platforminputcontexts imageformats iconengines xcbglintegrations; do
  [ -d "$QT_PLUGINS/$group" ] || continue
  mkdir -p "$APPDIR/usr/plugins/$group"
  cp -L "$QT_PLUGINS/$group"/*.so "$APPDIR/usr/plugins/$group/" 2>/dev/null || true
done
[ -f "$APPDIR/usr/plugins/platforms/libqxcb.so" ] \
  || { echo "✗ the xcb platform plugin is missing — the GUI could not start" >&2; exit 1; }

# 4. Bundle the shared libraries the binaries AND the plugins really need, read
#    out of ldd — never a guessed list.
#
#    Two things are deliberately NOT bundled:
#    · the glibc core + C++ runtime — these must come from the host, because the
#      host's dynamic loader is the one actually running us;
#    · the GL/driver stack (libGL, libEGL, libdrm, …) — those are tied to the
#      host's graphics driver, and shipping ours would break hardware GL.
CORE='^(ld-linux.*|libc|libm|libdl|libpthread|librt|libresolv|libgcc_s|libstdc\+\+)\.so'
DRIVER='^(libGL|libGLX|libGLdispatch|libOpenGL|libEGL|libdrm|libglapi|libgbm)\.so'

bundle_libs() { # <elf>
  ldd "$1" 2>/dev/null | awk '/=> \// {print $3}' | while read -r lib; do
    base="$(basename "$lib")"
    echo "$base" | grep -Eq "$CORE|$DRIVER" && continue
    [ -e "$APPDIR/usr/lib/$base" ] && continue
    cp -L "$lib" "$APPDIR/usr/lib/" 2>/dev/null || true
  done
}

# Iterate to a fixpoint: a freshly copied lib can pull in dependencies of its own
# (libQt6XcbQpa needs libQt6OpenGL, and so on), so keep sweeping until a pass
# adds nothing new.
sweep() {
  before=-1; after=0
  while [ "$before" != "$after" ]; do
    before="$(find "$APPDIR/usr/lib" -maxdepth 1 -type f | wc -l)"
    for elf in "$APPDIR/usr/bin"/* "$APPDIR/usr/lib"/*.so* "$APPDIR"/usr/plugins/*/*.so; do
      [ -f "$elf" ] && bundle_libs "$elf"
    done
    after="$(find "$APPDIR/usr/lib" -maxdepth 1 -type f | wc -l)"
  done
}
sweep
echo "  bundled $(find "$APPDIR/usr/lib" -maxdepth 1 -type f | wc -l) libraries"

# NOTE: the AppImage deliberately does NOT bundle QtWebEngine. Chromium adds ~90 MB
# (37 MB → ~120 MB) for a browser most users of a coding tool won't lean on, so the
# release ships the lightweight reader fallback. The full browser is a source build
# with qt6-webengine-dev, or the deb/rpm which pull it as a package dependency.

# 5. qt.conf makes Qt look for its plugins inside the AppDir instead of the build
#    machine's absolute paths, which is what it would otherwise bake in.
cat > "$APPDIR/usr/bin/qt.conf" <<'CONF'
[Paths]
Prefix = ..
Plugins = plugins
CONF

# 6. AppRun — one AppImage, two entry points.
cat > "$APPDIR/AppRun" <<'SH'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib:${LD_LIBRARY_PATH:-}"
export QT_PLUGIN_PATH="$HERE/usr/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$HERE/usr/plugins/platforms"
# The desktop app shells out to the agent CLI; point it at the copy we bundled
# rather than whatever happens to be on the user's PATH.
export OLLAMADEV_BINARY="$HERE/usr/bin/ollamadev"

# `--cli …` (or symlinking/renaming the AppImage to `ollamadev`) runs the agent
# CLI; anything else launches the desktop.
case "${1:-}" in
  --cli) shift; exec "$HERE/usr/bin/ollamadev" "$@" ;;
esac
if [ "$(basename "${ARGV0:-ollamadev-ade}")" = "ollamadev" ]; then
  exec "$HERE/usr/bin/ollamadev" "$@"
fi
exec "$HERE/usr/bin/ollamadev-ade" "$@"
SH
chmod +x "$APPDIR/AppRun"

# 7. Desktop entry + icon at the AppDir root, where appimagetool looks for them.
cp "$ROOT/packaging/ollamadev-ade.desktop" "$APPDIR/ollamadev-ade.desktop"
cp "$ROOT/packaging/ollamadev-ade.svg"     "$APPDIR/ollamadev-ade.svg"
cp "$ROOT/packaging/ollamadev-ade.svg"     "$APPDIR/.DirIcon"
mkdir -p "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/scalable/apps"
cp "$ROOT/packaging/ollamadev-ade.desktop" "$APPDIR/usr/share/applications/"
cp "$ROOT/packaging/ollamadev-ade.svg"     "$APPDIR/usr/share/icons/hicolor/scalable/apps/"

# 8. Report the glibc floor we actually got (see the header note).
FLOOR="$(objdump -T "$APPDIR/usr/bin/ollamadev-ade" "$APPDIR"/usr/lib/*.so* 2>/dev/null \
  | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -V | tail -1)"
echo "▸ glibc floor: ${FLOOR:-unknown} (runs on any host with glibc >= ${FLOOR:-?})"
case "$FLOOR" in
  2.3[0-5]|2.[12][0-9]) : ;;  # 2.35 or older — Ubuntu 22.04 / Mint 21 and up
  *) echo "  ⚠ built on a newer glibc than the 2.35 floor we ship — this AppImage"
     echo "    will NOT start on Ubuntu 22.04 / Mint 21. Build it on ubuntu-22.04"
     echo "    (that is what CI does) for a release artifact." ;;
esac

# 9. Pack. appimagetool is fetched on demand, exactly like the PHP project does.
mkdir -p "$DIST" "$ROOT/.build/tools"
TOOL="$ROOT/.build/tools/appimagetool-$AIARCH"
if [ ! -x "$TOOL" ]; then
  echo "▸ fetching appimagetool ($AIARCH)…"
  curl -fsSL -o "$TOOL" \
    "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$AIARCH.AppImage" \
    || { echo "✗ could not fetch appimagetool" >&2; exit 1; }
  chmod +x "$TOOL"
fi

OUT="$DIST/OllamaDev-$AIARCH.AppImage"
rm -f "$OUT"
# --appimage-extract-and-run: CI containers have no FUSE, and appimagetool is
# itself an AppImage that would otherwise fail to mount itself.
ARCH="$AIARCH" "$TOOL" --appimage-extract-and-run "$APPDIR" "$OUT" >/dev/null 2>&1 \
  || ARCH="$AIARCH" "$TOOL" "$APPDIR" "$OUT"

echo "✓ $OUT ($(du -h "$OUT" | cut -f1))"
