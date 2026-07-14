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
#include <QVector>

namespace odv {

// One hit. `redacted` is the ONLY view of the match anyone downstream ever gets:
// a finding travels into logs, the board, an auditor prompt and possibly a model
// running in the cloud, so it must never carry the credential it found.
struct Finding {
    QString rule;
    QString severity;  // high|med|low
    int line = 0;
    QString redacted;
    // Which file it is in. Only set by scanTree — a diff or a text scan already
    // knows what it was looking at, but a tree scan's finding is meaningless
    // without it ("line 4" of WHAT?).
    QString file;
};

// SECSCAN — dependency-free secret + unsafe-sink scanner. Catches hardcoded
// credentials before they land, in a commit or in a crew coder's branch. The
// crew Auditor uses it as a hard gate (a secret-bearing changeset never
// auto-lands); the `scan` command runs it on demand.
//
// Rules are tuned for precision over recall on purpose: a scanner that cries
// wolf gets switched off, and a switched-off scanner catches nothing.
class SecScan {
public:
    static QVector<Finding> scanText(const QString& text);

    // Unified diff: only ADDED lines are inspected, so findings describe what a
    // change INTRODUCES rather than what it merely touched. Line numbers are
    // resolved against the new file via the @@ hunk headers.
    static QVector<Finding> scanDiff(const QString& diff);

    // Skips binaries and very large files — neither is where a leaked key lives,
    // and both would only slow the commit gate down.
    static QVector<Finding> scanFile(const QString& path);

    // Every file under `dir`, recursively. `files` (when given) receives how many
    // were actually READ — which is the number that makes "clean" mean something.
    //
    // This exists because `ollamadev scan` with no argument defaults to the current
    // DIRECTORY, scanFile() only accepts a file, and so the natural way to run the
    // scanner examined nothing and printed "clean". A security tool that reports an
    // all-clear on a scan it never performed is worse than no security tool.
    //
    // Skips the places secrets are not and noise is: .git, node_modules, build
    // trees, vendored dependencies.
    static QVector<Finding> scanTree(const QString& dir, int* files = nullptr);
};

}  // namespace odv
