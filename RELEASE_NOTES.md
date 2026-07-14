# OllamaDev-Qt v0.2.0

A C++20 / Qt6 rewrite of [OllamaDev](https://github.com/kennethyork/OllamaDev). Two binaries
from one engine: **`ollamadev`** (the CLI) and **`ollamadev-ade`** (the desktop app — infinite
canvas, native terminals, crew board). Qt is the only dependency.

Broad and tested — 205 assertions in the self-test suite, every feature verified end-to-end. See "Known gaps" at the
bottom for what is deliberately out of scope in this release.

## What's new since v0.1.0

**The routing "brain".** The crew can auto-pick each role's model by difficulty — a small local
model for trivia, a strong/cloud model for hard design/debug work — instead of running everything
on one model.
- `ollamadev route [--run] "…"` — show (or run) which model the brain picks, and why.
- `ollamadev crew "…" --route` — auto-assign every role's model. The Director and Auditor reason
  *hard*, the Researcher *moderate*, and each coder is routed on *its own subtask's* difficulty.
- The desktop ** Brain** pane visualises the whole thing: all 14 crew faculties
  (Researcher → Router → Director → Roles → Skills → Coders → Auditor → Debate/Dedupe/Security →
  Secret gate → Overlap guard → Landing → Memory), a live classifier you type into, the running
  crew's routed model plan, and the token split.

**MDASH-style crew modes — all opt-in, the plain crew is unchanged.**
- `--debate` — advocate vs skeptic vs judge argue each changeset before it lands.
- `--dedupe` — hold coders whose work duplicates another's.
- `--security` — a read-only vulnerability hunt that writes a report (and shows scanners on the
  kanban board).
- `--swarm N` — raise the coder cap for a bigger fan-out.
- `--learn` — the crew compounds across runs: it folds past runs' memory + skills into its context
  before working, and distils what it learned into durable memory (and a reusable skill) after.

**Token-efficiency report.** Every crew run prints tokens per model and the **free-local vs
paid-cloud** split — the number that maps to a bill. The Brain pane shows it live.

**A real browser.** The Browser pane uses **QtWebEngine** (full Chromium — JS, CSS, media) when
built with it; without it, it falls back to a lightweight JavaScript-free reader so a lean build
still works. Build with `qt6-webengine-dev` installed for the full browser.

**Per-CLI terminals.** Add → CLI terminal opens a terminal straight into any installed CLI —
**ollamadev** (this app's own REPL) first, then Claude Code, Codex, Gemini, Cursor Agent, OpenCode,
Qwen Code. The desktop drives its own C++ `ollamadev`, resolved by path, never a stale build on
your PATH.

**Plus the full parity wave:** `pull`, `setup`, `config get/set`, `models presets/cloud/chain`,
session `load`/`resume`, `chat` threads, `completion` scripts, vision/`@image`, the `task`/subagent
tool, custom agent personas, output-styles, statusline, a usage meter, the context tuner, and all
the desktop panes (memory graph, crew topology, code-search, voice, agent-team, tasks, start) plus
the Ctrl-K command palette, the management dialogs, and the theme editor with 37 themes.

## Ollama and every major coding CLI, in parallel

Ollama is the default and stays first. Every other CLI is opt-in. Each crew role can be a
*different* model or backend — by hand or auto-routed — so one crew can mix Ollama, Claude, and
Codex at once (verified end-to-end).

| Provider | Concurrency | Why |
|---|---|---|
| Local Ollama, one GPU | ~2 | `OLLAMA_NUM_PARALLEL` slots; past that you queue inside Ollama and each slot costs a KV cache |
| Ollama cloud (`:cloud`) | ~8 | inference is on Ollama's servers, your GPU is idle |
| claude / codex / gemini / … | ~4 each | separate processes, separate remote APIs |

Run `ollamadev backends` for your machine's real numbers.

## Known gaps

The models themselves never get smarter — the `--learn` loop improves the *harness* (memory +
skills), not the weights. Release binaries ship the reader browser fallback; the full QtWebEngine
browser comes from a source build with `qt6-webengine-dev`. No macOS or Windows build in this
release (Linux only). The security-scan mode is scoped to this project's scale, not a 100-agent
harness.

## Install

Download the `.deb`, `.rpm`, `.AppImage`, or `.tar.gz` (built on Ubuntu 22.04, glibc 2.35 floor —
runs on Mint 21 and newer). From source:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/cli/ollamadev backends
```
