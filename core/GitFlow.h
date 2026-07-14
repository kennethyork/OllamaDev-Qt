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
#include <functional>

#include "Backend.h"
#include "SecScan.h"

namespace odv {

// Result of one git/gh invocation. `output` is stdout and stderr merged, because
// git says the interesting things (nothing to commit, rejected push) on stderr and
// a caller that only read stdout would report success on an empty string.
struct GitResult {
    int exit = -1;
    QString output;
    bool ok() const { return exit == 0; }
};

// Asked before an irreversible or remote-visible step (the commit, the push).
// Core never touches the terminal: the CLI answers this from stdin, the GUI from
// a dialog. Not installed => the step is declined, never assumed.
using Confirm = std::function<bool(const QString& prompt)>;

struct CommitOptions {
    bool stageAll = false;  // -a: stage every tracked+untracked change first
    QString message;        // -m: explicit message; skips the model entirely
    // --force is the ONLY key that opens the secret gate. See CommitResult::blocked.
    bool force = false;
    // --yes auto-answers the CONFIRMATION prompts (commit, push) for automation.
    // It is deliberately NOT a gate bypass — see GitFlow::commit().
    bool assumeYes = false;
    // `ship` shows the AI-written message and asks before committing; plain `commit`
    // just commits, so it leaves this false and passes no Confirm at all.
    bool askBeforeCommit = false;
    QString backendId;
    QString model;  // empty => git.model config key, else the session model
};

struct CommitResult {
    bool ok = false;
    // True when the secret scanner found a high-severity credential in the STAGED
    // diff and no --force was given. Distinct from a plain failure: nothing was
    // committed, but the tree is still staged and the user can fix or force.
    bool blocked = false;
    QVector<Finding> findings;
    QString message;  // the message we committed (AI-written unless -m)
    QString sha;
    QString error;
};

struct ShipResult {
    CommitResult commit;
    bool pushed = false;
    QString error;
};

struct PrReview {
    QString verdict;  // approve | comment | request_changes
    QString summary;
    QStringList findings;
};

// GITFLOW — closes the loop from "wrote code" to "shipped it": AI commit messages,
// PR drafting and PR review, on top of plain git.
//
// Every subprocess is spawned with an argv ARRAY. A composed shell string would
// make a branch name or a model-written commit message into executable syntax, and
// the commit message is attacker-influenced the moment the diff is (a repo you
// cloned can carry text that steers a small model). Messages go in over STDIN
// (`git commit -F -`) rather than argv for the same reason, and because a real
// commit body is multi-line and easily longer than a comfortable arg.
class GitFlow {
public:
    // Run git in the tool root with an argv array. `stdinText`, when non-empty, is
    // written to the child's stdin and the pipe closed (that is what `-F -` reads).
    // `env` entries are "KEY=VALUE", added to the inherited environment. This is
    // how an interactive rebase is driven WITHOUT an editor: GIT_SEQUENCE_EDITOR is
    // pointed at a command that drops our own todo list over git's, so the whole
    // rebase runs headless. Nothing else needs it.
    static GitResult git(const QStringList& args, const QString& stdinText = {},
                         const QStringList& env = {});

    static bool isRepo();
    static bool hasGh();  // the PR commands need it; everything else is local git

    static QString branch();
    static QString workingDiff();  // tracked vs HEAD + untracked as all-addition hunks
    static QString stagedDiff();

    // The model for the AI git workflow (commit message, PR draft, PR review).
    //
    // A DEDICATED `git.model` config key, falling back to the session model. This
    // split is intentional and worth keeping: a commit message is a small, boring,
    // high-frequency job that a 4B model does fine and instantly, while chat runs on
    // something much bigger. Paying the big model to write "fix: typo" is the tail
    // wagging the dog.
    static QString modelFor(const QString& fallbackModel);

    // Every high-severity finding the scanner sees in `diff`. The commit gate.
    static QVector<Finding> highFindings(const QString& diff);

    // Conventional Commit message for a staged diff. Empty if the model is unreachable.
    static QString commitMessage(const QString& diff, const QString& backendId,
                                 const QString& model, const CancelToken& cancel);

    // Stage (if asked) -> HARD secret gate -> message -> confirm -> commit.
    static CommitResult commit(const CommitOptions& o, const Confirm& confirm,
                               const CancelToken& cancel);

    // commit(), then ASK before pushing. The push is the first step that leaves the
    // machine, so it is never silent unless the caller asked for that with --yes.
    static ShipResult ship(const CommitOptions& o, const Confirm& confirm,
                           const CancelToken& cancel);

    // PR title + body from the branch's commits and diff.
    static bool prText(const QString& commits, const QString& diff, const QString& backendId,
                       const QString& model, QString* title, QString* body,
                       const CancelToken& cancel);

    // Model review of a diff: correctness, security, scope.
    static PrReview review(const QString& diff, const QString& backendId, const QString& model,
                           const CancelToken& cancel);
};

}  // namespace odv
