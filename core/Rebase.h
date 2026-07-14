#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

#include "Backend.h"

namespace odv {

// INTERACTIVE REBASE, driven headlessly — and an AI that proposes the plan.
//
// GitKraken lets you drag commits around to squash and reword them. This does the
// same job from the other end: it asks the model to read your messy history and
// propose the tidy-up, you review every line of it, and then it runs.
//
// HOW IT RUNS WITHOUT AN EDITOR. `git rebase -i` normally opens $EDITOR on a todo
// list. We instead point GIT_SEQUENCE_EDITOR at a command that copies OUR todo
// over git's, so the whole rebase is headless. And the two todo verbs that would
// still open an editor are avoided entirely:
//   * `reword` opens the editor → we use `pick` + `exec git commit --amend -F <file>`
//   * `squash` opens the editor → we use `fixup` (which keeps the first message),
//     plus the same exec trick when the combined message should change.
// So the plan runs to completion or it stops on a conflict. It never hangs waiting
// for a terminal that a GUI does not have.

struct RebaseStep {
    enum Action { Pick, Fixup, Drop, Reword };

    QString sha;
    QString subject;
    QString author;
    Action action = Pick;
    QString newMessage;  // Reword only

    QString actionName() const;
    static Action actionFromName(const QString& s);
};

struct RebasePlan {
    QString base;  // the commit to rebase onto ("--root", or a sha)
    QVector<RebaseStep> steps;  // OLDEST FIRST, which is git's todo order
    QString rationale;          // what the model says it did, for the human to read
};

struct RebaseResult {
    bool ok = false;
    bool conflicted = false;  // stopped mid-rebase; the user must resolve or abort
    QString output;
    QString backupRef;  // where HEAD was before, so this is always undoable
};

class Rebase {
public:
    // The commits from `base` (exclusive) up to HEAD, oldest first — the ones a
    // rebase would rewrite. `base` empty means "everything back to the root".
    static QVector<RebaseStep> commitsSince(const QString& base, int max = 50);

    // The last N commits, and the base they sit on ("--root" when they go all the
    // way back). The usual way in: "tidy up my last 10 commits".
    static RebasePlan planFor(int lastN);

    // Ask the model to propose a tidy-up: fold the "wip"/"fix typo" commits into
    // the thing they were fixing, reword the ones whose messages say nothing, drop
    // the ones that are pure noise. Returns the plan with its actions filled in.
    //
    // It never invents a commit and it never reorders — the plan is only ever a
    // per-commit verdict on the commits you actually have, which keeps the review
    // the human does honest.
    static RebasePlan propose(const RebasePlan& in, const QString& backendId,
                              const QString& model, const CancelToken& cancel);

    // Run it. Refuses on a dirty tree (a rebase over uncommitted work is a mess
    // nobody asked for), takes a backup ref FIRST, and reports a conflict rather
    // than pretending.
    static RebaseResult apply(const RebasePlan& plan, QString* err = nullptr);

    static bool inProgress();
    static bool abort();  // back to where you were, mid-conflict

    // Undo a finished rebase: move the branch back to the backup ref.
    static bool undo(const QString& backupRef, QString* err = nullptr);
};

}  // namespace odv
