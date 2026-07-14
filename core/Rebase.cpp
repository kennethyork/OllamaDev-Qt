#include "Rebase.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include "GitFlow.h"
#include "Json.h"
#include "Tools.h"

namespace odv {
namespace {

// Where the todo and the reword messages are written. Inside .git, not /tmp: the
// paths end up inside an `exec git commit -F <path>` line, and a world-writable
// directory is not where you want a file whose contents become a commit.
QString scratchDir() {
    const QString root = GitFlow::git({QStringLiteral("rev-parse"), QStringLiteral("--git-dir")})
                             .output.trimmed();
    if (root.isEmpty()) return {};
    const QString abs = QFileInfo(root).isAbsolute()
                            ? root
                            : Tools::threadRoot() + QLatin1Char('/') + root;
    const QString d = abs + QStringLiteral("/ollamadev-rebase");
    QDir().mkpath(d);
    return d;
}

bool writeFile(const QString& path, const QString& text) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(text.toUtf8()) >= 0;
}

}  // namespace

QString RebaseStep::actionName() const {
    switch (action) {
        case Fixup: return QStringLiteral("fixup");
        case Drop: return QStringLiteral("drop");
        case Reword: return QStringLiteral("reword");
        case Pick: break;
    }
    return QStringLiteral("pick");
}

RebaseStep::Action RebaseStep::actionFromName(const QString& s) {
    const QString t = s.trimmed().toLower();
    if (t == QLatin1String("fixup") || t == QLatin1String("squash")) return Fixup;
    if (t == QLatin1String("drop")) return Drop;
    if (t == QLatin1String("reword")) return Reword;
    return Pick;
}

QVector<RebaseStep> Rebase::commitsSince(const QString& base, int max) {
    QStringList args{QStringLiteral("--no-pager"), QStringLiteral("log"),
                     QStringLiteral("--reverse"),  // OLDEST first: git's todo order
                     QStringLiteral("--pretty=format:%H|%an|%s"), QStringLiteral("-n"),
                     QString::number(qBound(1, max, 200))};
    if (base.isEmpty() || base == QLatin1String("--root"))
        args << QStringLiteral("HEAD");
    else
        args << base + QStringLiteral("..HEAD");

    QVector<RebaseStep> out;
    for (const QString& line :
         GitFlow::git(args).output.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList f = line.split(QLatin1Char('|'));
        if (f.size() < 3) continue;
        RebaseStep s;
        s.sha = f.at(0);
        s.author = f.at(1);
        s.subject = QStringList(f.mid(2)).join(QLatin1Char('|'));  // a '|' in the subject is fine
        out.append(s);
    }
    return out;
}

RebasePlan Rebase::planFor(int lastN) {
    RebasePlan p;
    const int n = qBound(1, lastN, 100);
    // HEAD~n only exists when there ARE n ancestors. Fewer means we are rewriting
    // the whole history, which git spells --root.
    const GitResult r = GitFlow::git(
        {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
         QStringLiteral("HEAD~%1").arg(n)});
    p.base = r.ok() ? r.output.trimmed() : QStringLiteral("--root");
    p.steps = commitsSince(p.base, n);
    return p;
}

RebasePlan Rebase::propose(const RebasePlan& in, const QString& backendId, const QString& model,
                           const CancelToken& cancel) {
    RebasePlan out = in;
    auto backend = Backends::get(backendId);
    if (!backend || in.steps.isEmpty()) return out;

    // The model sees the messages AND the diffstat. Without the diffstat it cannot
    // tell a "fix typo" that belongs to the commit before it from one that belongs
    // to a different feature entirely, and folding the wrong pair together is
    // exactly the mistake that makes people distrust the whole feature.
    QString listing;
    for (int i = 0; i < in.steps.size(); ++i) {
        const RebaseStep& s = in.steps.at(i);
        const QString stat = GitFlow::git({QStringLiteral("--no-pager"), QStringLiteral("show"),
                                           QStringLiteral("--stat"),
                                           QStringLiteral("--pretty=format:"), s.sha})
                                 .output.trimmed();
        listing += QStringLiteral("%1. [%2] %3\n%4\n\n")
                       .arg(i + 1)
                       .arg(s.sha.left(8), s.subject, stat.left(400));
    }

    const QString sys = QStringLiteral(
        "You clean up messy git history before it is shared. You are given the commits in ORDER, "
        "OLDEST FIRST. For EACH one, choose exactly one action:\n"
        "  pick   — keep it as it is\n"
        "  fixup  — fold it into the commit ABOVE it (use this for 'wip', 'fix typo', 'oops', "
        "'address review' — the commits that only exist because the one before them was not "
        "finished)\n"
        "  reword — keep the change, replace the message (use this when the change is real but the "
        "message says nothing). Supply `message` as a Conventional Commit: a subject line, and a "
        "body only if it earns one.\n"
        "  drop   — delete it entirely. Use this ONLY for a commit that changes nothing that "
        "matters (a stray debug print that a later commit removes again).\n\n"
        "RULES YOU DO NOT BREAK:\n"
        "- The FIRST commit can never be `fixup` — there is nothing above it to fold into.\n"
        "- Only fold a commit into the one above it when they are the SAME piece of work. Look at "
        "the files. Two commits touching unrelated files are not a fixup, however scruffy the "
        "message.\n"
        "- When in doubt, `pick`. Leaving a commit alone is always safe; folding the wrong two "
        "together is not.\n"
        "- Return one entry per input commit, in the same order, and change NOTHING else.\n\n"
        "Reply with JSON only: {\"rationale\":\"one or two sentences\",\"steps\":[{\"sha\":\"...\","
        "\"action\":\"pick|fixup|reword|drop\",\"message\":\"only when action is reword\"}]}");

    const QJsonObject v = backend->chatJson(
        model,
        {{"system", sys, {}, {}, {}, {}, {}},
         {"user", QStringLiteral("Commits, oldest first:\n\n") + listing, {}, {}, {}, {}, {}}},
        cancel);

    const QJsonArray steps = v.value(QStringLiteral("steps")).toArray();
    if (steps.isEmpty()) return out;
    out.rationale = v.value(QStringLiteral("rationale")).toString();

    // Match by SHA, never by position: a model that drops or reorders an entry would
    // otherwise shift every verdict onto the wrong commit — which is how you squash
    // two things that had nothing to do with each other.
    QHash<QString, QJsonObject> bySha;
    for (const QJsonValue& x : steps) {
        const QJsonObject o = x.toObject();
        const QString sha = o.value(QStringLiteral("sha")).toString().trimmed();
        if (!sha.isEmpty()) bySha.insert(sha.left(8), o);
    }

    for (int i = 0; i < out.steps.size(); ++i) {
        RebaseStep& s = out.steps[i];
        const QJsonObject o = bySha.value(s.sha.left(8));
        if (o.isEmpty()) continue;  // the model forgot this one: leave it a pick
        s.action = RebaseStep::actionFromName(o.value(QStringLiteral("action")).toString());
        s.newMessage = o.value(QStringLiteral("message")).toString().trimmed();

        // The first commit has nothing above it to fold into. A model WILL propose
        // this, git would reject the whole todo, and the user would see a cryptic
        // failure instead of a plan.
        if (i == 0 && s.action == RebaseStep::Fixup) s.action = RebaseStep::Pick;
        // A reword with no message is a pick with extra steps.
        if (s.action == RebaseStep::Reword && s.newMessage.isEmpty()) s.action = RebaseStep::Pick;
    }
    return out;
}

bool Rebase::inProgress() {
    const QString gitDir = GitFlow::git({QStringLiteral("rev-parse"), QStringLiteral("--git-dir")})
                               .output.trimmed();
    if (gitDir.isEmpty()) return false;
    const QString abs =
        QFileInfo(gitDir).isAbsolute() ? gitDir : Tools::threadRoot() + QLatin1Char('/') + gitDir;
    return QFileInfo(abs + QStringLiteral("/rebase-merge")).exists() ||
           QFileInfo(abs + QStringLiteral("/rebase-apply")).exists();
}

bool Rebase::abort() {
    return GitFlow::git({QStringLiteral("rebase"), QStringLiteral("--abort")}).ok();
}

RebaseResult Rebase::apply(const RebasePlan& plan, QString* err) {
    RebaseResult r;
    const auto fail = [&](const QString& m) {
        if (err) *err = m;
        return r;
    };

    if (plan.steps.isEmpty()) return fail(QStringLiteral("nothing to do"));
    if (inProgress())
        return fail(QStringLiteral("a rebase is already in progress — finish or abort it first"));

    // A rebase over uncommitted work leaves you with a mess and a stash you will
    // forget about. But ONLY tracked changes matter: git itself does not mind
    // untracked files, and this app drops its own `.ollamadev/` session folder into
    // every project it touches — so checking those too made the app's own droppings
    // refuse the app's own rebase.
    if (!GitFlow::git({QStringLiteral("status"), QStringLiteral("--porcelain"),
                       QStringLiteral("--untracked-files=no")})
             .output.trimmed()
             .isEmpty())
        return fail(QStringLiteral(
            "commit or stash your changes first — a rebase cannot run over uncommitted work"));

    // Everything is a fixup? Then nothing survives, which is never what was meant.
    bool anyKept = false;
    for (const RebaseStep& s : plan.steps)
        if (s.action != RebaseStep::Drop) anyKept = true;
    if (!anyKept) return fail(QStringLiteral("that plan drops every commit"));

    const QString dir = scratchDir();
    if (dir.isEmpty()) return fail(QStringLiteral("not a git repository"));

    // THE BACKUP, taken BEFORE anything moves. A rebase rewrites history; the
    // reflog would let an expert recover it, and this lets everyone else. It is the
    // reason the Undo button can exist.
    const QString stamp = QString::number(QDateTime::currentSecsSinceEpoch());
    r.backupRef = QStringLiteral("refs/ollamadev/pre-rebase-%1").arg(stamp);
    const QString head = GitFlow::git({QStringLiteral("rev-parse"), QStringLiteral("HEAD")})
                             .output.trimmed();
    if (head.isEmpty()) return fail(QStringLiteral("no HEAD to rebase"));
    GitFlow::git({QStringLiteral("update-ref"), r.backupRef, head});

    // Build the todo. `reword` and `squash` would each open an editor, so neither is
    // ever emitted: a reword is a pick plus an `exec git commit --amend -F <file>`,
    // and a squash is a `fixup` (which keeps the first commit's message) plus the
    // same exec when the message should change. The rebase therefore runs headless.
    QStringList todo;
    int n = 0;
    for (const RebaseStep& s : plan.steps) {
        switch (s.action) {
            case RebaseStep::Drop:
                break;  // simply absent from the todo
            case RebaseStep::Fixup:
                todo << QStringLiteral("fixup %1").arg(s.sha);
                break;
            case RebaseStep::Reword: {
                const QString msgPath = dir + QStringLiteral("/msg-%1").arg(++n);
                if (!writeFile(msgPath, s.newMessage.trimmed() + QLatin1Char('\n')))
                    return fail(QStringLiteral("cannot write %1").arg(msgPath));
                todo << QStringLiteral("pick %1").arg(s.sha);
                // The path is ours (inside .git) and has no spaces, but quote it
                // anyway: this line IS a shell command to git.
                todo << QStringLiteral("exec git commit --amend --no-verify -F '%1'").arg(msgPath);
                break;
            }
            case RebaseStep::Pick:
                todo << QStringLiteral("pick %1").arg(s.sha);
                break;
        }
    }
    if (todo.isEmpty()) return fail(QStringLiteral("that plan leaves nothing to do"));

    const QString todoPath = dir + QStringLiteral("/todo");
    if (!writeFile(todoPath, todo.join(QLatin1Char('\n')) + QLatin1Char('\n')))
        return fail(QStringLiteral("cannot write %1").arg(todoPath));

    // GIT_SEQUENCE_EDITOR is run as `<cmd> <path-to-gits-todo>`, so `cp <ours>`
    // becomes `cp <ours> <gits>` and git reads back exactly the plan above.
    const QStringList env{
        QStringLiteral("GIT_SEQUENCE_EDITOR=cp '%1'").arg(todoPath),
        // Belt and braces: if some verb we did not expect still wants an editor,
        // `true` closes it immediately rather than hanging a GUI forever on a
        // terminal that does not exist.
        QStringLiteral("GIT_EDITOR=true"),
    };

    QStringList args{QStringLiteral("rebase"), QStringLiteral("-i")};
    if (plan.base == QLatin1String("--root") || plan.base.isEmpty())
        args << QStringLiteral("--root");
    else
        args << plan.base;

    const GitResult g = GitFlow::git(args, QString(), env);
    r.output = g.output.trimmed();
    r.ok = g.ok();
    r.conflicted = !g.ok() && inProgress();
    if (!r.ok && err)
        *err = r.conflicted
                   ? QStringLiteral("the rebase stopped on a conflict — resolve it, or abort")
                   : r.output;
    return r;
}

bool Rebase::undo(const QString& backupRef, QString* err) {
    if (backupRef.isEmpty()) {
        if (err) *err = QStringLiteral("no backup to go back to");
        return false;
    }
    if (inProgress()) abort();
    const GitResult r =
        GitFlow::git({QStringLiteral("reset"), QStringLiteral("--hard"), backupRef});
    if (!r.ok() && err) *err = r.output.trimmed();
    return r.ok();
}

}  // namespace odv
