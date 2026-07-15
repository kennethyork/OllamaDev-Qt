#!/usr/bin/env bash
# Brings up the headless display stack the GUI draws onto:
#   Xvfb  — a virtual X server (no physical monitor needed)
#   openbox — a minimal window manager so fullscreen/focus behave
#   x11vnc — exposes that display over VNC, on localhost only, password-gated
#
# The GUI itself is a SEPARATE service (ollamadev-desktop.service) so it can crash
# and restart without tearing down the display and dropping your VNC session.
set -euo pipefail

: "${DISPLAY_NUM:=:10}"
: "${GEOMETRY:=1600x900}"
: "${VNC_PORT:=5900}"

export DISPLAY="$DISPLAY_NUM"

# Kill our child processes (Xvfb, openbox, x11vnc) when this script exits, so a
# systemd restart doesn't leave a stale Xvfb holding the display lock.
cleanup() { pkill -P $$ >/dev/null 2>&1 || true; }
trap cleanup EXIT

# A leftover lock from an unclean shutdown makes Xvfb refuse the display.
rm -f "/tmp/.X${DISPLAY_NUM#:}-lock" 2>/dev/null || true

Xvfb "$DISPLAY_NUM" -screen 0 "${GEOMETRY}x24" -nolisten tcp &

# Wait for the X server to accept connections before starting anything on it.
for _ in $(seq 1 50); do
    xdpyinfo -display "$DISPLAY_NUM" >/dev/null 2>&1 && break
    sleep 0.2
done

openbox &

# x11vnc runs in the FOREGROUND and is this script's main process: while it lives
# the display is up; when it exits the EXIT trap tears down Xvfb + openbox and
# systemd restarts the unit clean.
#   -localhost: only websockify (also on localhost) may connect; the public edge
#     is Caddy.  -rfbauth: the VNC password from setup.sh.
#   -forever/-shared: survive client disconnects and allow a second device to join.
x11vnc -display "$DISPLAY_NUM" -forever -shared -localhost \
    -rfbport "$VNC_PORT" -rfbauth "$HOME/.vnc/passwd" \
    -noxdamage -quiet -o "$HOME/.vnc/x11vnc.log"
