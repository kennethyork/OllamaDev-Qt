// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace odv {

// Config-driven shell hooks fired at well-defined points, plus the user's own
// slash commands (prompt templates on disk). Both are opt-in: with nothing
// configured every entry point here is a no-op.
//
// SECURITY — WHERE HOOK CONFIG MAY COME FROM.
// A hook is an arbitrary shell command that we execute. Config::get() layers a
// REPO-LOCAL ./.ollamadev.json in with the home config, so reading hooks through
// it would mean that merely running `ollamadev` inside a cloned hostile repo
// executes that repo's command on the next tool call. Hooks therefore read from
// the HOME config only (~/.ollamadev/config.json, ~/.config/ollamadev/config.json,
// ~/.ollamadev/ade-prefs.json) — never Config::get, never the project file. The
// PHP original made the same split (Config::trustedGet); it is preserved here
// deliberately. Do not "simplify" this to Config::get.
class Hooks {
public:
    struct Hook {
        QString command;
        QString matcher;  // regex tested against the subject (tool name); empty = always
    };

    static QStringList knownEvents();

    // Canonical spelling for case-insensitive input, or an empty string if unknown.
    static QString normalizeEvent(const QString& e);

    static QVector<Hook> listFor(const QString& event);
    static bool add(const QString& event, const QString& command, const QString& matcher = {});
    static bool removeAt(const QString& event, int index);

    // PreToolUse: a hook exiting NON-ZERO BLOCKS the tool, and its output is the
    // reason handed back to the model.
    //
    // FAILS CLOSED. If the hook cannot be spawned at all, or hangs past its
    // timeout, the tool is BLOCKED — not allowed. A gate that opens when it breaks
    // is not a gate, and users install these to stop a model from doing something
    // (rm -rf, a push to prod). Returns true when the tool must be blocked.
    static bool preToolUse(const QString& tool, const QJsonObject& params, QString* reason);

    // PostToolUse: informational, never blocks.
    static void postToolUse(const QString& tool, const QJsonObject& params, const QString& result);

    // Generic events (UserPromptSubmit, SessionStart, Stop, PreCompact,
    // SubagentStop, Notification). The payload goes to the hook on stdin as JSON.
    // Non-blocking; a failure is swallowed.
    static void event(const QString& name, const QJsonObject& payload = {});

    // The two original string-arg events (beforePrompt, afterEdit): the args are
    // appended to the command line, shell-quoted.
    static void run(const QString& event, const QStringList& args = {});

    // `ollamadev hooks [list|add <event> <cmd> [--match <re>]|remove <event> <i>]`.
    // Returns the text to print. `words` are the words AFTER "hooks".
    static QString editorCommand(const QStringList& words);
    static QString renderConfigured();
};

// User-defined slash commands: prompt templates in a `commands/` directory under
// the project (./.ollamadev/commands, which shadows) or the home
// (~/.ollamadev/commands). `<name>.md|.txt|.prompt` holds the template; $ARGS is
// the whole argument string and $1 $2 … are the positional words.
//
// Unlike a hook, a template is only ever fed to the MODEL as a prompt — it is not
// executed — so reading one from the project directory is not the same risk and
// the project copy is allowed to shadow the personal one, as in the PHP original.
class UserCmds {
public:
    static bool exists(const QString& name);

    // The expanded prompt, or an empty string if there is no such command.
    static QString expand(const QString& name, const QString& args);

    static QStringList listAll();  // deduped, project shadowing home
    static QString render();       // human-readable listing
};

}  // namespace odv
