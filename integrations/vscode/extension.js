// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A thin bridge, on purpose: it does NOT reimplement the agent in TypeScript. It
// hands your task to the same `ollamadev` CLI the desktop app drives, in an
// integrated terminal rooted at your workspace — so the crew's parallel coders,
// audit and landing all behave exactly as they do everywhere else, and you never
// leave VS Code to kick one off.

const vscode = require("vscode");

// One reused terminal so successive tasks stack in a single panel instead of
// spawning a new one each time.
let term = null;
function terminal() {
  if (!term || term.exitStatus !== undefined) {
    term = vscode.window.createTerminal("OllamaDev");
  }
  term.show();
  return term;
}

function cli() {
  return vscode.workspace.getConfiguration("ollamadev").get("path", "ollamadev");
}

// Shell-quote a single argument for POSIX shells (the integrated terminal).
function q(s) {
  return "'" + String(s).replace(/'/g, "'\\''") + "'";
}

function run(args) {
  terminal().sendText(cli() + " " + args.map(q).join(" "));
}

// Prompt for a task, optionally seeded with a note about the selection, and fan it
// out to the crew.
async function crew() {
  const editor = vscode.window.activeTextEditor;
  const sel = editor && !editor.selection.isEmpty ? editor.document.getText(editor.selection) : "";
  const hint = sel
    ? `\n\nContext (selected in ${editor.document.fileName}):\n${sel}`
    : "";
  const task = await vscode.window.showInputBox({
    prompt: "Task for the OllamaDev crew",
    placeHolder: 'e.g. "add unit tests for the parser"',
  });
  if (!task) return;
  run(["crew", task + hint]);
}

// Send the selection to the single agent as a question about that code.
async function askSelection() {
  const editor = vscode.window.activeTextEditor;
  if (!editor || editor.selection.isEmpty) {
    vscode.window.showInformationMessage("Select some code first.");
    return;
  }
  const code = editor.document.getText(editor.selection);
  const question = await vscode.window.showInputBox({
    prompt: "Ask OllamaDev about the selection",
    placeHolder: "e.g. what does this do? / find the bug",
  });
  if (!question) return;
  run([`${question}\n\n${code}`]);
}

// Just open the interactive agent in the workspace folder.
function chatHere() {
  terminal().sendText(cli());
}

function activate(context) {
  context.subscriptions.push(
    vscode.commands.registerCommand("ollamadev.crew", crew),
    vscode.commands.registerCommand("ollamadev.askSelection", askSelection),
    vscode.commands.registerCommand("ollamadev.chatHere", chatHere)
  );
}

function deactivate() {}

module.exports = { activate, deactivate };
