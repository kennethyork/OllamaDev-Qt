# OllamaDev-Qt v0.1.0 — pre-release

A C++20 / Qt6 rewrite of [OllamaDev](https://github.com/kennethyork/OllamaDev) (PHP CLI +
BosonPHP webview desktop app). Two binaries out of one engine:

- **`ollamadev`** — the CLI. Qt Core + Network, no GUI dependency.
- **`ollamadev-ade`** — the desktop app. Infinite canvas, native terminals, crew board.

**This is a pre-release.** The engine is better than the PHP original in ways that are measured
below; it also has a few hours of mileage against the original's months. Treat it as something to
try, not yet as a drop-in replacement.

## Dependencies

Qt 6 and a C++20 compiler. That is the entire list — no third-party libraries anywhere, which is
the same constraint the PHP version held, carried across:

- `QRegularExpression` **is** PCRE2, so the tool-call parsers and secret scanner port directly.
- `QJsonDocument` replaces a JSON library, `QProcess` drives the coding CLIs.
- `forkpty()` gives us the terminal — no `script(1)`, no libvterm, no QTermWidget.

## Ollama and every major coding CLI, in parallel

Ollama is the default and stays first — it is the only local, free, offline-capable provider, and
the reason the project exists. Everything else is opt-in.

```
Backend        Installed  Native tools  Concurrency
Ollama         yes        yes           2 local / 8 cloud
Claude Code    yes        own loop      4
Codex          yes        own loop      4
Gemini CLI     yes        own loop      4
Cursor Agent   yes        own loop      4
OpenCode       yes        own loop      4
Qwen Code      yes        own loop      4
Aider · Goose · Amp · Crush · Droid — wired, gated on a PATH probe
```

"own loop" means the CLI does its own agentic work and its own file edits; we hand it a subtask and
a sandbox and let it run. Routing is per role and per coder, so **one crew can mix providers**:

```sh
ollamadev crew "add OAuth and write the tests" \
  --coder-backends ollama,claude,codex --coder-models qwen3.5:9b,,
```

## Parallelism is per backend, because the constraint is per backend

This is the part worth being precise about. "Run N coders at once" means very different things
depending on who does the inference:

| Provider | Real limit | Why |
|---|---|---|
| Local Ollama, one GPU | ~2 | One `ollama serve` exposes `OLLAMA_NUM_PARALLEL` slots per loaded model. Past that, requests queue *inside Ollama* — you do not go faster, and each extra slot costs a full `num_ctx` KV cache of VRAM. |
| Ollama cloud (`:cloud`) | ~8 | Inference runs on Ollama's servers. Your GPU is not involved. |
| claude / codex / gemini / … | ~4 each | Separate processes, separate remote APIs. Bounded by rate limits, not your hardware. |

So four *local* coders is roughly 2×, not 4×. Four *cloud* coders genuinely is ~4×. A permit
limiter keyed on backend + locality enforces this, so a mixed crew throttles each provider
independently. Run `ollamadev backends` for your machine's real numbers.

## What is better than the PHP version

- **The crew is actually parallel.** PHP's parallel mode exists but is off by default (light mode
  defaults on and silently gates it), and even enabled it is a wave pool that stalls on the slowest
  coder in each chunk. Here every coder is a thread and the throttle is admission control.
- **The serial overhead is gone.** Sandbox copies (N full tree copies, all in the parent *before
  any coder started*), changeset capture (byte-comparing every file of both trees), and the N
  audits were all serial. Now threaded, with a size+mtime prefilter.
- **Real diffs.** The PHP `fileDiff` was not a diff — it emitted every old line as `-` and every
  new line as `+`, then truncated to 16 KB before the auditor saw it, so on a large file the
  auditor could literally miss the change. Now a Myers diff, verified by round-tripping through
  `git apply`.
- **Real terminals.** PHP shelled out to `script(1)`, bridged the pty through two files polled
  every 12–150 ms, and resized it by walking `/proc/<pid>/fd/0` to run `stty -F`. Here:
  `forkpty()`, a `QSocketNotifier`, and `ioctl(TIOCSWINSZ)`. Event-driven, no polling.
- **A second instance no longer kills the first one's terminals.** The PHP app ran
  `pkill -9 -f '__pty-daemon__'` and `rm -rf ~/.ollamadev/terminals/*` on every boot. Every child
  here is owned by handle, and a test fails the build if anyone reintroduces a kill-by-name.

## Still no git

Each coder works in a copied sandbox and produces a changeset (a manifest plus a mirror of the
changed files). Accepting one copies those files into your folder, creating any missing
directories. No worktrees, no branches, no merges — the crew runs in any folder, repo or not.

Overlap between coders is caught at the path level, first-writer-wins; the loser is held with its
diff intact rather than merged. A changeset that introduces a credential is never auto-applied, no
matter how clean the audit was.

## Known gaps

Not yet ported from the PHP app: chat threads, checkpoints/undo, workspaces in the CLI, subagents,
vision, plugins, tmux panes, self-update, the desktop's full theme set and voice-command grammar.
There is no Windows build yet.

## Install

Download the AppImage (built on Ubuntu 22.04, so glibc 2.35 and newer), or the `.deb`/`.tar.gz`.
Building from source needs `qt6-base-dev` and CMake 3.21+:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/cli/ollamadev backends
```
