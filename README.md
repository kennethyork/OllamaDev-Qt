# OllamaDev (C++ / Qt)

A feature-for-feature rewrite of [OllamaDev](../OllamaDev) — the PHP CLI + BosonPHP webview
desktop app — in C++20 and Qt 6.

Two binaries out of one core:

| | |
|---|---|
| `ollamadev` | the CLI. Qt Core + Network only, no GUI dependency. |
| `ollamadev-ade` | the desktop app. Qt Widgets. Infinite canvas, real terminals, crew board. |

## Dependencies

Qt 6 (Core, Network, Concurrent, Widgets) and a C++20 compiler. That is the whole list.

There is no third-party library anywhere in this repo, and that is deliberate — it is the same
constraint the PHP version held ("vanilla PHP/HTML/CSS/JS, BosonPHP the only dependency"), carried
across:

- `QJsonDocument` replaces a JSON library.
- `QRegularExpression` **is** PCRE2, so the tool-call parsers and the secret scanner port directly.
- `QProcess` drives the coding CLIs.
- `QNetworkAccessManager` talks to Ollama.
- `forkpty()` gives us the terminal. No `script`, no libvterm, no QTermWidget.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/cli/ollamadev backends
./build/ade/ollamadev-ade
```

## What changed, and why

### 1. The crew is actually parallel now

The PHP crew *had* a parallel mode (`pcntl_fork` in `array_chunk` waves), but it was off by
default — light mode is on by default for local models, and light mode silently disables
`--parallel`. So out of the box, coders ran one after another.

Even with it on, it was a **wave pool**: fork a chunk, `waitpid` every child in that chunk, then
start the next chunk. One slow coder stalled the whole wave.

Here, every coder is a thread from the start, and the throttle is admission control rather than
batching (`core/Parallel.h`). A fast coder picks up the next subtask while a slow one is still
thinking.

### 2. Parallelism is per-backend, because the constraint is per-backend

This is the part worth being precise about, because "run 6 coders at once" means very different
things depending on who is doing the inference:

| Provider | Real limit | Why |
|---|---|---|
| **Local Ollama, one GPU** | ~2 | One `ollama serve` exposes `OLLAMA_NUM_PARALLEL` slots per loaded model. Beyond that, requests queue *inside Ollama* — you do not go faster, and each extra slot costs a full `num_ctx` KV cache out of VRAM. |
| **Ollama cloud** (`:cloud`) | ~8 | Inference happens on Ollama's servers. Your GPU is not involved, so there is no local contention to respect. |
| **claude / codex / gemini / …** | ~4 each | Separate processes hitting separate remote APIs. Bounded by rate limits, not by your hardware. |

`Limiter` (`core/Parallel.h`) hands out permits keyed by `backend + locality`, so a crew that mixes
one local coder with three cloud coders throttles each independently instead of all-or-nothing. On
a 24 GB RTX 3090 with `qwen3.5:9b`, four local coders is not 4× — it is roughly 2×. Four **cloud**
coders genuinely is ~4×. That asymmetry is now expressed in code rather than left to the user.

Run `ollamadev backends` to see the real numbers for your machine.

### 3. The orchestration work that *was* serial is now threaded

Independent of inference, the PHP version did a lot of avoidable serial work:

- **Sandbox creation** — one full recursive copy of the project per coder, all of them in the
  parent process *before any coder started*. Now parallel (`Sandbox::copyTree`).
- **Changeset capture** — read every file of both trees and byte-compare, per coder. Now
  size+mtime prefiltered.
- **Auditing** — N independent model calls in a serial `foreach`. Now a parallel fan-out.
- **Diffs** — the PHP `fileDiff` was not a diff at all; it emitted every old line as `-` and every
  new line as `+`. On a large file that "diff" then got truncated to 16 KB before the auditor saw
  it, so the auditor could literally miss the change. `Sandbox::unifiedDiff` is a real diff.

### 4. Ollama *and* every major coding CLI

`IModelBackend` (`core/Backend.h`) is the seam. Ollama speaks HTTP with native function calling;
the coding CLIs are headless subprocesses that run *their own* agent loop and do *their own* file
edits. The crew does not care which is which — it hands a coder a subtask and a sandbox.

Backends: `ollama`, `claude`, `codex`, `gemini`, `cursor-agent`, `opencode`, `qwen`, `aider`,
`goose`, `amp`, `crush`, `droid`.

Because routing is per role and per coder, a single crew can mix them:

```sh
ollamadev crew "add OAuth and write the tests" \
  --director-backend ollama --director-model gpt-oss:20b-cloud \
  --coder-backends claude,codex,ollama \
  --auditor-backend claude
```

### 5. No git, still

Carried over unchanged: each coder works in a **copied sandbox**, produces a **changeset**
(a manifest plus a mirror of the changed files), and accepting one copies those files into your
folder — creating any missing directories. There are no worktrees, branches, or merges, and the
crew runs in any folder whether or not it is a repo.

Overlap between coders is caught at the path level, first-writer-wins, and the loser is held with
its diff intact rather than merged.

### 6. Things that get simpler in C++, for free

- **The PTY.** The PHP app shelled out to `script -qfc`, bridged the terminal through two files
  (`pty-in` / `pty-out`) that the UI polled every 12–150 ms, and resized it by walking
  `/proc/<pid>/fd/0` to find the pts and shelling out to `stty -F`. Here: `forkpty()`, a
  `QSocketNotifier`, and `ioctl(TIOCSWINSZ)`. Event-driven, zero polling, no `/proc` spelunking.
- **The clipboard.** The webview needed a three-tier paste fallback (a hidden textarea, a
  WebKitGTK-only flag, and a native `pbpaste`/`Get-Clipboard` binding) purely to work around
  browser clipboard restrictions. Now it is `QClipboard`.
- **Terminal state.** The webview app killed every PTY on boot and could never restore a live
  session. A native app owns its child processes.

## Status

Core engine and CLI first, desktop app alongside it. See `tests/smoke.cpp` for what is verified.
