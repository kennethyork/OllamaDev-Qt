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
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace odv {

// A changeset: what a coder actually changed, captured by comparing its
// sandbox against the project. Durable, so a human can accept it hours later.
//
// On disk under <store>/:
//   files/<rel path>   full contents of every created/modified file
//   manifest.json      {created:[], modified:[], deleted:[], projectRoot:""}
//   diff.txt           unified diff, for review and the secret scanner
struct Changeset {
    QStringList created;
    QStringList modified;
    QStringList deleted;
    QString diff;
    QString store;
    bool empty() const { return created.isEmpty() && modified.isEmpty() && deleted.isEmpty(); }
    QStringList files() const { return created + modified + deleted; }
};

class Sandbox {
public:
    // Directories never copied into a sandbox nor compared.
    static QStringList excludes();

    // Make a coder's sandbox at `dest`, and take it away again.
    //
    // A GIT WORKTREE when the project is a git repo, a full folder copy when it is
    // not. The worktree is the better deal on every axis that matters:
    //   * it SHARES THE OBJECT STORE, so N coders cost N working trees rather than
    //     N working trees plus N copies of .git (on this repo that is 143MB of
    //     history, per coder, copied for nothing);
    //   * the sandbox is a REAL GIT REPO, so a coder can actually run git_diff /
    //     git_log / git_status. Under the old folder copy .git was excluded, which
    //     meant the seventeen git_* tools were dead inside a crew coder — they
    //     answered "not a git repository" and the coder had no way to see history;
    //   * gitignored junk (build/, dist/) is simply absent, instead of being copied
    //     into every sandbox and then having to be excluded from the capture.
    //
    // THE SUBTLETY, and the reason this is not a two-line change: `git worktree add`
    // checks out a COMMIT, but the coder must start from the user's WORKING TREE.
    // Anything uncommitted would otherwise be invisible to the coder — and worse,
    // capture() would compare the sandbox against the project, see the user's own
    // uncommitted edits, and record them as changes the CODER had reverted. So the
    // dirty state (modified, added, deleted, untracked) is replicated into the
    // worktree afterwards. The coder ends up looking at exactly what the user sees,
    // which is what the folder copy did, and what capture() assumes.
    static bool create(const QString& projectRoot, const QString& dest, QString* err = nullptr);
    static bool destroy(const QString& projectRoot, const QString& dest);

    // Full recursive copy. Parallelised across files — this is pure I/O and was
    // one of the serial bottlenecks in the PHP version. Still used verbatim for a
    // project that is not under git.
    static bool copyTree(const QString& src, const QString& dst, QString* err = nullptr);
    static bool removeTree(const QString& dir);

    // rel path -> absolute, leaves only, excludes applied at any depth.
    static QHash<QString, QString> listFiles(const QString& root);

    // Compare `sandbox` against `projectRoot`, write the changeset to `storeDir`.
    static Changeset capture(const QString& projectRoot, const QString& sandbox,
                             const QString& storeDir);

    // Reload a changeset previously written by capture() from its store, without
    // any sandbox. `crew resume` uses this: a coder that already finished keeps
    // its changeset on disk, so the run re-audits and lands it rather than
    // re-running the model. Returns an empty Changeset if the store is missing.
    static Changeset load(const QString& storeDir);

    // Copy a stored changeset into the project. Creates parent folders.
    // This is what "accept" runs — it is the only thing that ever writes to the
    // user's tree.
    static bool apply(const QString& storeDir, const QString& projectRoot,
                      QStringList* wrote = nullptr, QString* err = nullptr);

    // Real unified diff (Myers), not a whole-file replacement hunk. Keeps the
    // auditor's context budget usable on large files.
    static QString unifiedDiff(const QString& path, const QString& oldText,
                               const QString& newText, int context = 3);
};

}  // namespace odv
