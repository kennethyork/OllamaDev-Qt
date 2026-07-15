#!/usr/bin/env bash
# End-to-end smoke test for the VPS deployment path — WITHOUT a VPS.
#
# It stands up the same stack setup.sh would (a headless Xvfb, the GUI running
# --fullscreen on it, then x11vnc + noVNC in front) on throwaway display/ports,
# asserts each layer actually works, screenshots the running app as proof, and
# tears everything down. Run it before touching a real server, and in CI, so the
# "runs while my computer is off" path can't rot unnoticed.
#
# Layered + skip-friendly, like the C++ smoke tests: the app-on-headless-X core
# always runs; the VNC/noVNC layers SKIP (not fail) when those tools aren't
# installed, so this is useful on a dev box and thorough on a provisioned VPS.
#
#   deploy/vps/smoke-test.sh
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
APP="${ODV_APP:-$ROOT/build/ade/ollamadev-ade}"
DISP=":101"                     # throwaway display, away from :0 and the real :10
GEOMETRY="1280x800"
VNC_PORT=59010                  # throwaway ports, away from the real 5900/6080
NOVNC_PORT=60810
SHOT="${TMPDIR:-/tmp}/odv-vps-smoke.png"

pass=0 fail=0 skip=0
ok()   { echo "  ok    $1"; pass=$((pass + 1)); }
bad()  { echo "  FAIL  $1"; fail=$((fail + 1)); }
skipc(){ echo "  skip  $1"; skip=$((skip + 1)); }

pids=()
cleanup() {
    for p in "${pids[@]:-}"; do kill "$p" >/dev/null 2>&1 || true; done
    rm -f "/tmp/.X${DISP#:}-lock" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "▸ VPS deploy smoke test  (app: $APP)"

# --- 0. the binary must exist -------------------------------------------------
if [ ! -x "$APP" ]; then
    bad "GUI binary not found at $APP — build it first (cmake --build build)"
    echo "$fail failed"; exit 1
fi
command -v Xvfb >/dev/null || { bad "Xvfb not installed"; exit 1; }

# --- 1. headless display comes up --------------------------------------------
rm -f "/tmp/.X${DISP#:}-lock" 2>/dev/null || true
Xvfb "$DISP" -screen 0 "${GEOMETRY}x24" -nolisten tcp >/dev/null 2>&1 &
pids+=($!)
for _ in $(seq 1 50); do xdpyinfo -display "$DISP" >/dev/null 2>&1 && break; sleep 0.2; done
if xdpyinfo -display "$DISP" >/dev/null 2>&1; then
    ok "Xvfb headless display is up ($GEOMETRY)"
else
    bad "Xvfb never accepted connections"; exit 1
fi

# A window manager so showFullScreen() actually fills the screen — fullscreen is a
# WM-cooperative state (_NET_WM_STATE_FULLSCREEN), so without one the window keeps
# its natural size. The VPS runs openbox (display-session.sh); start it here too
# when available so the fullscreen check is meaningful.
have_wm=0
if command -v openbox >/dev/null 2>&1; then
    DISPLAY="$DISP" openbox >/dev/null 2>&1 &
    pids+=($!)
    sleep 1
    have_wm=1
fi

# --- 2. the GUI launches --fullscreen and paints a window --------------------
# Throwaway HOME so the test neither reads nor overwrites the real workspace state.
THOME="${TMPDIR:-/tmp}/odv-vps-smoke-home"
rm -rf "$THOME"; mkdir -p "$THOME"
DISPLAY="$DISP" HOME="$THOME" "$APP" --fullscreen >/dev/null 2>&1 &
pids+=($!)

# Pick the MAIN window, not a helper: the app maps a couple of tiny (10x10) utility
# windows alongside the real one, so choose the widest window among its matches.
mainwin_geom() {  # echoes "<winid> <width>" for the widest ollamadev-ade window
    local best="" bestw=0 wid w
    # Match by title ("OllamaDev ADE — …"): the main window's WM_CLASS instance is
    # "OllamaDev ADE" while a tiny helper carries the "ollamadev-ade" class, so a
    # class search would find only the 10px helper.
    for wid in $(DISPLAY="$DISP" xdotool search --name "OllamaDev ADE" 2>/dev/null); do
        w="$(DISPLAY="$DISP" xdotool getwindowgeometry "$wid" 2>/dev/null | awk '/Geometry/{print $2}')"
        w="${w%x*}"
        [ -n "$w" ] && [ "$w" -gt "$bestw" ] && { bestw="$w"; best="$wid"; }
    done
    [ -n "$best" ] && echo "$best $bestw"
}

appwin="" appw=0
for _ in $(seq 1 60); do
    read -r appwin appw <<<"$(mainwin_geom)"
    [ -n "$appwin" ] && [ "${appw:-0}" -ge 200 ] && break  # >200 == the real window, not a 10px helper
    sleep 0.25
done
if [ -n "$appwin" ] && [ "${appw:-0}" -ge 200 ]; then
    ok "GUI launched --fullscreen and mapped its main window"
else
    bad "GUI main window never appeared on the headless display"; exit 1
fi

# The main window should fill the screen — but only a WM can grant fullscreen, so
# skip the extent check when none is running (the VPS always has openbox).
scr_w="${GEOMETRY%x*}"
if [ "$have_wm" = 1 ]; then
    if [ "${appw:-0}" -ge "$((scr_w - 40))" ]; then
        ok "the window fills the screen (fullscreen: ${appw}/${scr_w}px)"
    else
        bad "the window did not go fullscreen (${appw}/${scr_w}px)"
    fi
else
    skipc "fullscreen-extent check (no window manager here; the VPS runs openbox)"
fi

# proof-of-render screenshot
if command -v import >/dev/null 2>&1; then
    DISPLAY="$DISP" import -window root "$SHOT" >/dev/null 2>&1 \
        && [ -s "$SHOT" ] && ok "captured a screenshot of the running app ($SHOT)" \
        || bad "screenshot came back empty — the app may not be painting"
else
    skipc "screenshot (ImageMagick 'import' not installed)"
fi

# --- 3. VNC layer (x11vnc) — only if installed -------------------------------
if command -v x11vnc >/dev/null 2>&1; then
    vncpass="${TMPDIR:-/tmp}/odv-vps-smoke.vncpass"
    x11vnc -storepasswd smoketest "$vncpass" >/dev/null 2>&1
    x11vnc -display "$DISP" -rfbport "$VNC_PORT" -localhost -forever -shared \
           -rfbauth "$vncpass" -quiet -bg >/dev/null 2>&1
    sleep 1
    if (exec 3<>"/dev/tcp/127.0.0.1/$VNC_PORT") 2>/dev/null; then
        ok "x11vnc is serving the display on localhost:$VNC_PORT"
        exec 3>&- 2>/dev/null || true
    else
        bad "x11vnc did not open port $VNC_PORT"
    fi
    pkill -f "rfbport $VNC_PORT" >/dev/null 2>&1 || true
    rm -f "$vncpass"
else
    skipc "VNC layer (x11vnc not installed — installed by setup.sh on the VPS)"
fi

# --- 4. browser bridge (noVNC / websockify) — only if installed --------------
if command -v websockify >/dev/null 2>&1 && [ -f /usr/share/novnc/vnc.html ]; then
    websockify --web=/usr/share/novnc/ "127.0.0.1:$NOVNC_PORT" "127.0.0.1:$VNC_PORT" \
        >/dev/null 2>&1 &
    pids+=($!)
    served=""
    for _ in $(seq 1 20); do
        code="$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$NOVNC_PORT/vnc.html" 2>/dev/null)"
        [ "$code" = "200" ] && { served=1; break; }
        sleep 0.25
    done
    [ -n "$served" ] && ok "noVNC serves vnc.html to the browser on localhost:$NOVNC_PORT" \
                     || bad "noVNC did not serve vnc.html (HTTP ${code:-none})"
else
    skipc "browser bridge (websockify/noVNC not installed — installed by setup.sh)"
fi

echo ""
echo "$pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
