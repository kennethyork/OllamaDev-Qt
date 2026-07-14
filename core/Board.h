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

#include <optional>

namespace odv {

// One thing waiting for a human. Three kinds land here:
//   permission   — a mutating tool wants to run; the agent blocks until you decide
//   crew_branch  — a coder's changeset is ready; you accept (files land in your
//                  folder) or deny (the changeset is discarded)
//   checkpoint   — the agent took a checkpoint; deny rolls back to it
struct Decision {
    QString id;
    QString kind;      // "crew_branch" | "permission" | "checkpoint"
    QString summary;
    QString detail;    // e.g. the diff
    QJsonObject data;  // {runId, n, repoRoot, store, reason, files}
    qint64 ts = 0;     // unix seconds, set at enqueue
    QString verdict;   // "" while pending, then "accept" | "deny"
};

// A file-based decision queue shared by the CLI and the GUI. Both processes are
// pollers over the same two files in ~/.ollamadev/board:
//
//   decisions.jsonl  append-only log — the source of truth and the audit trail
//   current.json     derived index of what is pending + the last decided ones
//
// No sockets, no daemon, no DB: the CLI can post a decision and the GUI (a
// different process, started later) can answer it. Every mutation takes a
// QLockFile and rewrites current.json atomically, so a concurrent writer can
// never be read half-written.
class Board {
public:
    // The verdicts a UI may record. "always" and "skip" exist for the
    // permission gate (allow-this-tool-forever / skip-this-call); everything
    // that is not "accept"/"always" blocks the caller.
    static QStringList verdicts();

    static QString enqueue(const Decision& d);  // returns id (generated when d.id is empty)
    static QVector<Decision> pending();         // oldest first — UIs number them 1..N
    static QVector<Decision> recent(int limit = 20);  // newest first
    static bool decide(const QString& id, const QString& verdict, QString* err = nullptr);
    static std::optional<Decision> get(const QString& id);
    static void clear();

    // Blocking wait used by the permission gate. Polls every 250ms.
    // Returns the verdict, or "timeout" when timeoutSeconds elapses (0 = wait
    // forever). Fails closed: a decision that vanished reads as "deny".
    static QString waitFor(const QString& id, int timeoutSeconds = 0);

    static QString logFile();    // <boardDir>/decisions.jsonl
    static QString indexFile();  // <boardDir>/current.json
};

}  // namespace odv
