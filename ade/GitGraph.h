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

// One commit, with the lane geometry needed to draw the graph.
struct GraphCommit {
    QString sha;
    QStringList parents;
    QString author;
    QString date;
    QString subject;
    QStringList refs;  // "main", "origin/main", "tag: v1.0", "HEAD -> main"
    bool isHead = false;

    // Assigned by layout():
    int lane = 0;                  // which column this commit's dot sits in
    QVector<QPair<int, int>> links;  // (fromLane, toLane) edges passing through this row
    int lanesWide = 1;             // how many lanes are live at this row
};

// The commit graph — the thing that makes a git client feel like a git client.
//
// Lane assignment is the classic "keep a list of open branch tips" sweep: walk the
// commits newest-first; a commit takes the lane of whichever open tip is waiting
// for it (or a fresh lane if nothing is), then its parents replace it — the first
// parent inherits the lane so a straight line stays straight, and any second
// parent (a merge) opens a lane of its own.
//
// It is O(commits x lanes) and honest about what it is: a topological drawing of
// what `git log --parents` said, not an opinion about it.
class GitGraph {
public:
    // `git log --all --parents --date=short --pretty=%H|%P|%an|%ad|%D|%s`
    static QVector<GraphCommit> parse(const QString& logOutput);

    // Fills in lane / links / lanesWide. Returns the widest row.
    static int layout(QVector<GraphCommit>& commits);

    // Deliberately NO colour here. GitGraph is pure layout — which commit sits in
    // which lane — and that is testable without a GUI. The palette lives with the
    // thing that paints it, so a headless build (ODV_BUILD_ADE=OFF) never has to
    // drag in QtGui just to run the tests.
};

}  // namespace odv
