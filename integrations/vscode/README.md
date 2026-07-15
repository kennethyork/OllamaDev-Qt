# OllamaDev Bridge for VS Code

Kick off the OllamaDev **crew** (or ask the single agent) from inside VS Code —
right-click in the editor, describe the task, and it runs in an integrated
terminal rooted at your workspace. It's a thin bridge over the same `ollamadev`
CLI the desktop app uses, so the crew's parallel coders / audit / landing behave
identically; you just don't have to leave your editor to start one.

## Why a bridge, not a rewrite

The agent, the crew, the sandboxing and the model routing all live in the
`ollamadev` binary. Reimplementing any of that in the extension would mean two
codebases drifting apart. So this extension only *launches* the CLI — one source
of truth, everywhere.

## Commands

| Command | What it does |
|---|---|
| **OllamaDev: Send task to crew…** | Prompts for a task (seeded with your selection as context) and runs `ollamadev crew "<task>"` |
| **OllamaDev: Ask about selection** | Sends the selected code + your question to the single agent |
| **OllamaDev: Open agent in this folder** | Opens the interactive `ollamadev` REPL in the workspace |

The first two are also on the editor right-click menu.

## Install (no build step — it's plain JS)

```bash
# link it into your VS Code extensions folder
ln -s "$(pwd)/integrations/vscode" ~/.vscode/extensions/ollamadev-bridge
# then reload VS Code
```

Or press **F5** in this folder from VS Code to launch an Extension Development
Host with it loaded.

## Settings

- `ollamadev.path` — path to the CLI (defaults to `ollamadev` on your PATH). If
  you installed via `install.sh` it's `~/.local/bin/ollamadev`.

## Requirements

The `ollamadev` CLI on your PATH (or set `ollamadev.path`), and Ollama running
with a model — same as the rest of OllamaDev.
