#!/usr/bin/env bash
# One-shot VPS provisioner for running the WHOLE OllamaDev GUI on a server,
# reachable from any device through a browser. Targets Ubuntu/Debian (apt); for
# Fedora/Arch swap the package-install block — everything else is distro-neutral.
#
# Run as root on a FRESH VPS:   sudo ./setup.sh
# It installs packages, creates the run user, installs the systemd units, and then
# prints the few secrets you must set by hand (VNC password, web password, domain).
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then echo "run as root: sudo $0" >&2; exit 1; fi

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
. "$HERE/ollamadev-vps.env"

echo "==> Installing packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -y
# Display stack + VNC + browser bridge, plus the Qt6 runtime the GUI links against.
apt-get install -y \
    xvfb x11vnc openbox x11-utils \
    novnc websockify \
    libqt6widgets6 libqt6network6 libqt6gui6 libqt6core6 \
    fonts-dejavu-core ca-certificates curl git

echo "==> Installing Caddy (public TLS + auth front door)"
if ! command -v caddy >/dev/null 2>&1; then
    apt-get install -y debian-keyring debian-archive-keyring apt-transport-https
    curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' \
        | gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
    curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' \
        | tee /etc/apt/sources.list.d/caddy-stable.list >/dev/null
    apt-get update -y && apt-get install -y caddy
fi

echo "==> Installing Ollama (localhost only)"
if ! command -v ollama >/dev/null 2>&1; then
    curl -fsSL https://ollama.com/install.sh | sh
fi

echo "==> Creating run user '$ODV_USER'"
if ! id "$ODV_USER" >/dev/null 2>&1; then
    useradd --create-home --shell /bin/bash "$ODV_USER"
fi
install -d -o "$ODV_USER" -g "$ODV_USER" "$ODV_HOME/.vnc" "$ODV_WORKDIR"

echo "==> Installing config + scripts + services"
install -d /etc/ollamadev /opt/ollamadev/deploy/vps
install -m 0644 "$HERE/ollamadev-vps.env"      /etc/ollamadev/ollamadev-vps.env
install -m 0755 "$HERE/display-session.sh"     /opt/ollamadev/deploy/vps/display-session.sh
install -m 0644 "$HERE/ollamadev-display.service" /etc/systemd/system/
install -m 0644 "$HERE/ollamadev-desktop.service" /etc/systemd/system/
install -m 0644 "$HERE/ollamadev-novnc.service"   /etc/systemd/system/
systemctl daemon-reload

cat <<EOF

============================================================================
 Packages + services installed. THREE manual steps remain (secrets + build):
============================================================================

 1. Build the GUI on this VPS (as $ODV_USER), so \$ODV_APP exists:
      sudo -u $ODV_USER -i
      git clone <your repo> ~/ollamadev-qt && cd ~/ollamadev-qt
      cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j\$(nproc)
      exit
    (Needs the -dev packages; see README if the configure step complains.)

 2. Set the VNC password (as $ODV_USER):
      sudo -u $ODV_USER x11vnc -storepasswd ~/.vnc/passwd

 3. Set the public domain + web password in the Caddyfile:
      caddy hash-password           # copy the hash
      edit deploy/vps/Caddyfile     # set your domain + username + hash
      cp deploy/vps/Caddyfile /etc/caddy/Caddyfile

 Then start it all:
      systemctl enable --now ollama ollamadev-display ollamadev-desktop ollamadev-novnc caddy

 Open https://your-domain/vnc.html  — or, with no domain, use the SSH tunnel in
 the README. Full walkthrough + security notes: deploy/vps/README.md
============================================================================
EOF
