# Run the whole OllamaDev app on a VPS — 24/7, from any device

This runs the **entire canvas GUI** on a server so it keeps working while your own
computer is off. You connect from a browser (laptop, phone, tablet) to the same
live session. Everything — the canvas, every CLI/terminal pane, the crew, and
Ollama — runs on the VPS.

Because the app is a real desktop (Qt) program, "run it in the cloud" means giving
the server a **headless screen** and streaming that screen to your browser. That is
all this kit does, wrapped so it survives reboots and is locked down.

```
  your browser  ──TLS+password──▶  Caddy  ──▶  noVNC  ──▶  x11vnc  ──▶  Xvfb (virtual screen)
   (any device)                   :443        :6080        :5900         │
                                                                         ▼
                                                          ollamadev-ade (the GUI, fullscreen)
                                                                         │  talks to
                                                                         ▼
                                                          Ollama  (127.0.0.1:11434, never public)
```

## Does everything work in this mode? Yes.

- **The canvas GUI** — runs fullscreen on the virtual screen; you see and drive it
  in the browser, mouse and keyboard included.
- **All the CLI / terminal panes** — these are ordinary PTYs the app spawns *on the
  VPS*. They run the VPS's `ollamadev` binary against the VPS's local Ollama, so
  they behave exactly as they do on your desktop. Nothing about them is tied to a
  physical display.
- **The crew / agent jobs** — run on the VPS and keep going whether or not any
  browser is connected. Close the tab, shut your laptop; the work continues.
- **Session + canvas resume** — the app's own persistence still applies, so a
  service restart reopens your panes where you left them.

## What you need

- A VPS (2+ vCPU, 4 GB+ RAM; more if you run big models on CPU, or a GPU box for
  speed). Ubuntu 22.04/24.04 or Debian 12 assumed.
- A domain name pointed at the VPS **if** you want browser access over TLS. No
  domain? Use the SSH-tunnel option below — no domain, no open ports, still secure.

## Install

```bash
git clone <your repo> ollamadev-qt
cd ollamadev-qt/deploy/vps
# edit ollamadev-vps.env if you want a different user / screen size / project path
sudo ./setup.sh
```

`setup.sh` installs the display stack (Xvfb, openbox, x11vnc), the browser bridge
(noVNC/websockify), Caddy, and Ollama; creates the run user; and installs the
systemd units. It then prints the **three manual steps** it deliberately does not
automate (they involve secrets or a repo URL):

1. **Build the GUI on the VPS** so `$ODV_APP` exists (the `cmake` two-liner).
2. **Set the VNC password:** `sudo -u ollamadev x11vnc -storepasswd ~/.vnc/passwd`
3. **Set your domain + web password** in `Caddyfile` (`caddy hash-password`), then
   copy it to `/etc/caddy/Caddyfile`.

Then bring it all up:

```bash
sudo systemctl enable --now ollama ollamadev-display ollamadev-desktop ollamadev-novnc caddy
```

Open **`https://your-domain/vnc.html`**, enter the web password (Caddy), then the
VNC password, and the canvas is there.

### No domain? SSH tunnel instead (nothing public)

Skip Caddy entirely. Keep noVNC on localhost and forward it over SSH:

```bash
ssh -N -L 6080:127.0.0.1:6080 ollamadev@YOUR_VPS_IP
# then on your machine, open:
http://localhost:6080/vnc.html
```

Only SSH (port 22) is exposed; the app rides inside the encrypted tunnel. This is
the simplest secure option and needs no domain or certificate.

## The pieces (systemd units)

| Unit | What it does |
|---|---|
| `ollama.service` | The model server, bound to `127.0.0.1` — never exposed. Installed by Ollama. |
| `ollamadev-display.service` | Xvfb + openbox + x11vnc — the headless screen. Runs `display-session.sh`. |
| `ollamadev-desktop.service` | The GUI itself, fullscreen, on that screen. Restarts on crash without dropping the display. |
| `ollamadev-novnc.service` | websockify — serves the screen to the browser over WebSocket, on localhost. |
| `caddy.service` | The only public listener: TLS + password, proxying to noVNC. (Omit if tunnelling.) |

Handy: `journalctl -u ollamadev-desktop -f` to watch the GUI, and
`~/.vnc/x11vnc.log` for the VNC server.

## Security model

- **Ollama is never public.** It listens on `127.0.0.1`; only the co-located GUI
  and CLI panes reach it. (This is why the local path needs no auth token — the
  new `ollama.host` / `Auth token` fields in Settings are for the *opposite* case:
  a laptop reaching a remote Ollama across the network.)
- **Two gates on the browser path:** Caddy basic-auth (or the SSH tunnel) in front,
  then the VNC password. VNC and noVNC bind to localhost, so neither is directly
  reachable from the internet.
- **TLS** is automatic via Caddy/Let's Encrypt when you use a domain.
- Run it as the unprivileged `ollamadev` user (setup.sh does). Do not run the GUI
  as root.

## Tuning

- **Screen size:** `GEOMETRY` in `ollamadev-vps.env` (then
  `systemctl restart ollamadev-display ollamadev-desktop`). noVNC rescales in the
  browser, so this is just the desktop working area.
- **Which project opens:** `ODV_WORKDIR`. The app also resumes its last workspace,
  so after the first launch this mostly sets the default.
- **Performance:** CPU-only inference is slow for large models; pick a small model,
  or use a GPU VPS and install the CUDA build of Ollama.
