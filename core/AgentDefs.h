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
#include <QString>
#include <QStringList>
#include <QVector>
namespace odv {
// File-defined agent personas (.ollamadev/agents/*.md), output styles, statusline.
struct AgentDef {
    QString name, description, model, permission, prompt, file;
    QStringList tools;
    bool isNull() const { return name.isEmpty(); }
};
class AgentDefs {
public:
    static QStringList list();          // persona names, sorted
    static QVector<AgentDef> all();     // every persona (project shadows home)
    static AgentDef get(const QString& name);
};
class OutputStyles {
public:
    // The prompt suffix for a named style, appended to the system prompt. Empty
    // for "default" (and for any unknown name).
    static QString suffix(const QString& style);
    static QStringList names();
    static QString current();               // the configured style (Config outputStyle)
    static bool set(const QString& name);   // persist it; false for an unknown name
    static QString describe(const QString& style);
};
class StatusLine {
public:
    static bool configured();
    // {model}{cwd}{branch}{mode} template, OR a shell command whose first stdout
    // line is shown. Config is read from HOME only — never a repo-local file.
    static QString render(const QString& model = {}, const QString& mode = {});
};
}  // namespace odv
