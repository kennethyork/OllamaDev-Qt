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
#include <QVector>

namespace odv {

// A workspace is a BOOKMARKED PROJECT FOLDER — one directory, plus an opaque blob
// the desktop uses to restore its canvas.
//
// Deliberately global (~/.ollamadev/workspaces.json), not per-repo: the whole
// point is that it spans projects. Everything else in the app — sessions, memory,
// the crew — is keyed off the working directory, so "switching workspace" is
// really just "be in that directory", and the rest follows.
struct Workspace {
    QString id;    // ws_<sha1(abspath)[0:10]> — derived from the path, so re-adding
                   // the same folder updates in place instead of duplicating it
    QString name;  // defaults to the folder's basename
    QString path;  // absolute
    QString lastOpened;  // ISO-8601
    // The desktop's canvas layout. Core NEVER interprets this — but it must be
    // preserved byte for byte, or a CLI `ws add` would silently wipe the window
    // layout the GUI saved.
    QJsonObject state;
};

class Workspaces {
public:
    static QVector<Workspace> all();
    static QString activeId();

    // Upsert by resolved absolute path. Returns the entry (new or updated) and
    // makes it active. An empty `name` keeps the existing one.
    static Workspace add(const QString& path, const QString& name = {});

    // id, then absolute path, then name (case-insensitively). First hit wins.
    static bool find(const QString& key, Workspace* out);

    // If the removed workspace was the active one, the first remaining entry
    // becomes active rather than leaving a dangling id.
    static bool remove(const QString& key);

    // Mark it active and bump lastOpened. Returns its path, or empty if unknown.
    static QString open(const QString& key);

    // The desktop's canvas layout, kept opaque. Unknown id = no-op.
    static bool saveState(const QString& id, const QJsonObject& state);
};

}  // namespace odv
