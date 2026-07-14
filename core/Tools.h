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
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>

namespace odv {

// What a tool hands back to the model. `error` is only meaningful when !ok; the
// agent folds it into the tool-role message so the model can recover from it.
struct ToolResult {
    bool ok = true;
    QString output;
    QString error;
};

using ToolFn = std::function<ToolResult(const QJsonObject& args)>;

struct ToolDef {
    QString name;
    QString description;
    QJsonObject parameters;  // JSON Schema, as sent to the model
    // Does running this tool change state on disk / in the world? This single
    // bit is the whole permission model: read-only tools never prompt and never
    // block, mutating tools are what ReadOnly/Plan/Ask gate on.
    bool mutates = false;
    ToolFn fn;
};

enum class PermMode { Auto, Ask, ReadOnly, Plan };

// The gate enforced by Tools::run(). Deliberately tiny: mode + interactivity +
// two optional callbacks so the CLI can prompt on stdin and the GUI can pop a
// dialog without core ever touching a terminal.
class Permission {
public:
    static void setMode(PermMode m);
    static PermMode mode();
    static void setInteractive(bool on);
    static bool interactive();

    // Returns true if the tool may run.
    //   Auto      — everything runs (this is the Crew coder context: an isolated
    //               sandbox, non-interactive, so it must never block).
    //   ReadOnly  — every mutates=true tool is denied.
    //   Plan      — same as ReadOnly. exit_plan_mode is mutates=false on purpose:
    //               it is the ONE tool allowed to change state (the mode) in plan
    //               mode, and it only does so with explicit user approval.
    //   Ask       — read-only runs; a mutation asks the approver. With interactive
    //               off there is nobody to ask, so mutations are denied.
    static bool check(const ToolDef& t, const QJsonObject& args);

    // Ask-mode approver. Not installed => Ask+interactive denies mutations rather
    // than silently allowing them.
    static void setAsker(std::function<bool(const ToolDef&, const QJsonObject&)> fn);

    // Plan-mode approver, called by exit_plan_mode with the proposed plan. Only a
    // true return leaves plan mode — a model cannot self-approve, because in a
    // non-interactive run no approver is installed.
    static void setPlanApprover(std::function<bool(const QString& plan)> fn);
    static bool approvePlan(const QString& plan);

    // Leave plan mode, restoring whatever mode preceded it. Returns the new mode.
    static PermMode exitPlan();

    static QString modeName(PermMode m);
    static PermMode modeFromName(const QString& s);
};

class Tools {
public:
    static void registerAll();  // idempotent, thread-safe

    static const ToolDef* find(const QString& name);

    // Native function-calling schemas, in registration order.
    //
    // QUIRK — a tool with no parameters MUST emit "properties": {} (an empty JSON
    // OBJECT), never [] (an empty array). Ollama >= 0.23 rejects the whole request
    // with HTTP 400 ("Value looks like object, but can't find closing '}' symbol")
    // and tool-calling silently dies for every model in that request. QJsonObject
    // serializes to {} correctly — never substitute a QJsonArray here.
    static QJsonArray schemas();

    static ToolResult run(const QString& name, const QJsonObject& args);

    static QStringList names();

    // Root that relative paths resolve against, for THIS thread only.
    // Crew coders run in parallel on separate threads, each in its own sandbox
    // copy, so this cannot be process-wide cwd — QDir::setCurrent() would have
    // them stomping each other's working directory and writing into the wrong
    // sandbox. Defaults to the process cwd when unset.
    static void setThreadRoot(const QString& root);
    static QString threadRoot();

    // Was a root set EXPLICITLY on this thread (as opposed to falling back to the
    // process cwd)? Agent needs this to propagate a crew coder's sandbox onto the
    // worker threads it fans read-only tools out to — thread_local does not
    // inherit — without accidentally turning the plain CLI's cwd into a jail.
    static bool hasThreadRoot();

    // ask_user's channel to the human. The CLI installs a stdin prompt; a GUI can
    // install a dialog. Returns the answer, or an empty string if the human
    // declined to answer.
    //
    // Not installed, or a non-interactive run: there is NOBODY to ask (a crew coder
    // runs on a worker thread with no terminal), so ask_user must not block — it
    // tells the model to proceed on a stated assumption instead. A tool that can
    // hang a whole run waiting for an answer that can never come is worse than no
    // tool at all.
    static void setQuestionAsker(
        std::function<QString(const QString& question, const QStringList& options)> fn);
    static QString askQuestion(const QString& question, const QStringList& options, bool* asked);

    // Resolve a possibly-relative tool argument against threadRoot().
    //
    // When a thread root has been set EXPLICITLY (i.e. we are a sandboxed crew
    // coder) this is a security boundary, not a convenience: anything that
    // escapes the root — an absolute path outside it, or ".." traversal that
    // climbs out after normalisation — is rejected, so a coder can never write
    // outside its sandbox. When no root was set (the plain CLI / ADE session,
    // where the user's own cwd IS the project and absolute paths like /etc/hosts
    // are legitimate) paths are resolved but not confined.
    static QString resolvePath(const QString& p, bool* ok = nullptr);
};

}  // namespace odv
