// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "GitFlow.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

#include "Config.h"
#include "Json.h"
#include "Tools.h"

namespace odv {
namespace {

// git/gh are not fast, but they are not slow either; a hang here would wedge the
// REPL, so every call is bounded. Generous enough for a large `git add -A`.
constexpr int kGitTimeoutMs = 120000;

// The diff we hand a model. Enough to characterise a change; past this we are
// paying for tokens that do not move the summary.
constexpr int kDiffBudget = 12000;
constexpr int kReviewBudget = 14000;

// A CLI that edits files must run where the tools run, not in the process cwd.
// A crew coder is a thread with its own sandbox root, and a `git commit` that
// ignored that would reach out of the sandbox and commit in the user's real repo.
QString runRoot() { return Tools::threadRoot(); }

GitResult runTool(const QString& program, const QStringList& args, const QString& stdinText,
                  const QStringList& env = {}) {
    GitResult r;
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);  // argv array: no shell, so nothing here can be syntax
    p.setWorkingDirectory(runRoot());
    if (!env.isEmpty()) {
        QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
        for (const QString& kv : env) {
            const int eq = kv.indexOf(QLatin1Char('='));
            if (eq > 0) e.insert(kv.left(eq), kv.mid(eq + 1));
        }
        p.setProcessEnvironment(e);
    }
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start();
    if (!p.waitForStarted(10000)) {
        r.output = QStringLiteral("%1 is not installed").arg(program);
        return r;
    }
    if (!stdinText.isEmpty()) p.write(stdinText.toUtf8());
    p.closeWriteChannel();  // `-F -` blocks until stdin sees EOF
    if (!p.waitForFinished(kGitTimeoutMs)) {
        p.kill();  // our own child, by handle — never a kill-by-name
        p.waitForFinished(2000);
        r.output = QStringLiteral("%1 timed out").arg(program);
        return r;
    }
    r.exit = p.exitCode();
    r.output = QString::fromUtf8(p.readAll());
    return r;
}

QVector<ChatMessage> promptPair(const QString& system, const QString& user) {
    QVector<ChatMessage> m;
    ChatMessage s;
    s.role = QStringLiteral("system");
    s.content = system;
    ChatMessage u;
    u.role = QStringLiteral("user");
    u.content = user;
    m << s << u;
    return m;
}

QJsonObject askJson(const QString& backendId, const QString& model, const QString& system,
                    const QString& user, const CancelToken& cancel) {
    const BackendPtr be = Backends::get(backendId.isEmpty() ? QStringLiteral("ollama") : backendId);
    if (!be) return {};
    return be->chatJson(model, promptPair(system, user), cancel);
}

}  // namespace

GitResult GitFlow::git(const QStringList& args, const QString& stdinText, const QStringList& env) {
    return runTool(QStringLiteral("git"), args, stdinText, env);
}

bool GitFlow::isRepo() {
    // Verified against `git rev-parse --help`.
    const GitResult r = git({QStringLiteral("rev-parse"), QStringLiteral("--is-inside-work-tree")});
    return r.ok() && r.output.trimmed() == QLatin1String("true");
}

bool GitFlow::hasGh() { return !QStandardPaths::findExecutable(QStringLiteral("gh")).isEmpty(); }

QString GitFlow::branch() {
    const GitResult r =
        git({QStringLiteral("rev-parse"), QStringLiteral("--abbrev-ref"), QStringLiteral("HEAD")});
    return r.ok() ? r.output.trimmed() : QString();
}

QString GitFlow::workingDiff() {
    if (!isRepo()) return {};

    // An empty repo has no HEAD to diff against; `git diff HEAD` there is an error,
    // not an empty diff, and the first commit would show as no changes at all.
    const bool hasHead =
        git({QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("HEAD")}).ok();

    QStringList args{QStringLiteral("--no-pager"), QStringLiteral("diff")};
    if (hasHead) args << QStringLiteral("HEAD");
    QString diff = git(args).output;

    // Untracked files are the whole point of a review diff — new work lives there —
    // but `git diff` never shows them. Render each as an all-addition diff.
    const GitResult un = git({QStringLiteral("ls-files"), QStringLiteral("--others"),
                              QStringLiteral("--exclude-standard")});
    if (un.ok()) {
        const QStringList files = un.output.split('\n', Qt::SkipEmptyParts);
        for (const QString& f : files) {
            const QString rel = f.trimmed();
            if (rel.isEmpty()) continue;
            // --no-index against /dev/null: "everything in this file is new". It exits
            // 1 whenever the two sides differ, which for a new file is always, so the
            // exit code carries no information here and we take the output regardless.
            const GitResult d = git({QStringLiteral("--no-pager"), QStringLiteral("diff"),
                                     QStringLiteral("--no-index"), QStringLiteral("--"),
                                     QStringLiteral("/dev/null"), rel});
            diff += d.output;
        }
    }
    return diff;
}

QString GitFlow::stagedDiff() {
    if (!isRepo()) return {};
    // --cached is the staged-vs-HEAD diff (--staged is its synonym). This, not the
    // working tree, is what a commit will actually contain — and therefore the only
    // honest thing to scan for secrets and to summarise.
    return git({QStringLiteral("--no-pager"), QStringLiteral("diff"), QStringLiteral("--cached")})
        .output;
}

QString GitFlow::modelFor(const QString& fallbackModel) {
    // An explicit git.model always wins. (This is the user's own setting — e.g.
    // gpt-oss:120b-cloud — and it is never second-guessed.)
    const QString m = Config::str(QStringLiteral("git.model")).trimmed();
    if (!m.isEmpty()) return m;

    // No preference set: a commit message, a PR body, a PR review are small, boring,
    // high-frequency jobs. gpt-oss:20b does them well and — unlike the session model,
    // which is often a big cloud tag — it runs LOCALLY, so the tail (a "fix: typo"
    // message) does not wag the dog. Prefer it when the user actually has it; fall
    // back to the session model otherwise, so a fresh install is never broken by a
    // default pointing at a model nobody pulled.
    if (auto ollama = Backends::get(QStringLiteral("ollama"))) {
        const QStringList have = ollama->models();
        for (const QString& tag : {QStringLiteral("gpt-oss:20b"), QStringLiteral("gpt-oss:20b-cloud")})
            if (have.contains(tag)) return tag;
    }
    return fallbackModel;
}

QVector<Finding> GitFlow::highFindings(const QString& diff) {
    QVector<Finding> high;
    for (const Finding& f : SecScan::scanDiff(diff))
        if (f.severity == QLatin1String("high")) high << f;
    return high;
}

QString GitFlow::commitMessage(const QString& diff, const QString& backendId, const QString& model,
                               const CancelToken& cancel) {
    const QString sys = QStringLiteral(
        "Write ONE Conventional Commit message for this staged diff. Output ONLY JSON: "
        "{\"message\":\"type(scope): summary\\n\\n- optional bullet\\n- optional bullet\"}. "
        "Subject line <=72 chars, imperative mood, no trailing period. Types: feat, fix, docs, "
        "refactor, test, chore, perf, build, ci. Bullets only if they add information.");
    const QJsonObject j = askJson(backendId, model, sys,
                                  QStringLiteral("Diff:\n") + diff.left(kDiffBudget), cancel);
    return j.value(QStringLiteral("message")).toString().trimmed();
}

CommitResult GitFlow::commit(const CommitOptions& o, const Confirm& confirm,
                             const CancelToken& cancel) {
    CommitResult res;
    if (!isRepo()) {
        res.error = QStringLiteral("not a git repository");
        return res;
    }

    if (o.stageAll) {
        // -A stages modifications, additions AND deletions, including untracked
        // files. `git commit -a` alone would silently leave new files behind.
        const GitResult add = git({QStringLiteral("add"), QStringLiteral("-A")});
        if (!add.ok()) {
            res.error = QStringLiteral("git add failed: %1").arg(add.output.trimmed());
            return res;
        }
    }

    const QString staged = stagedDiff();
    if (staged.trimmed().isEmpty()) {
        res.error = QStringLiteral("nothing staged to commit");
        return res;
    }

    // ---- HARD SECRET GATE -------------------------------------------------
    // Scanned on the STAGED diff, because that is what would actually land. A
    // high-severity finding stops the commit dead.
    //
    // Only --force opens this gate. --yes does NOT, and that distinction is the
    // whole point of having two flags: --yes exists so CI can run unattended, and
    // an unattended run is precisely the one that must never be able to push a
    // live credential to a remote by answering its own prompt. Automation gets to
    // skip the QUESTIONS; a human has to make the DECISION to override a leak.
    res.findings = highFindings(staged);
    if (!res.findings.isEmpty() && !o.force) {
        res.blocked = true;
        res.error = QStringLiteral("%1 high-severity secret finding(s) in the staged diff")
                        .arg(res.findings.size());
        return res;
    }

    res.message = o.message.trimmed();
    if (res.message.isEmpty()) {
        const QString model = modelFor(o.model);
        res.message = commitMessage(staged, o.backendId, model, cancel);
        if (cancel.cancelled()) {
            res.error = QStringLiteral("cancelled");
            return res;
        }
        if (res.message.isEmpty()) {
            res.error = QStringLiteral(
                "could not generate a commit message (is the model reachable?) — pass -m");
            return res;
        }
    }

    if (o.askBeforeCommit && !o.assumeYes) {
        // The message travels IN the prompt: a caller that had to ask for it
        // separately could ask the user to approve a message it never showed them.
        const QString prompt =
            QStringLiteral("\n%1\n\nCommit with this message?").arg(res.message);
        if (!confirm || !confirm(prompt)) {
            res.error = QStringLiteral("cancelled");
            return res;
        }
    }

    // -F - reads the message from stdin: a real message is multi-line, and keeping
    // it out of argv keeps model-written text away from any arg parsing at all.
    const GitResult c =
        git({QStringLiteral("commit"), QStringLiteral("-F"), QStringLiteral("-")}, res.message);
    if (!c.ok()) {
        res.error = QStringLiteral("git commit failed: %1").arg(c.output.trimmed());
        return res;
    }

    const GitResult sha =
        git({QStringLiteral("rev-parse"), QStringLiteral("--short"), QStringLiteral("HEAD")});
    res.sha = sha.ok() ? sha.output.trimmed() : QString();
    res.ok = true;
    return res;
}

ShipResult GitFlow::ship(const CommitOptions& o, const Confirm& confirm,
                         const CancelToken& cancel) {
    ShipResult res;
    res.commit = commit(o, confirm, cancel);
    if (!res.commit.ok) {
        res.error = res.commit.error;
        return res;
    }
    if (cancel.cancelled()) {
        res.error = QStringLiteral("cancelled");
        return res;
    }

    // The push is the first step that leaves this machine and the first that cannot
    // be undone quietly, so it is always asked for — unless --yes said not to.
    //
    // The sha goes IN the prompt: by now the commit has landed, and asking "push?"
    // without saying so leaves the user deciding about a commit they were never told
    // succeeded.
    if (!o.assumeYes) {
        const QString prompt =
            QStringLiteral("Committed %1. Push to the remote?").arg(res.commit.sha);
        if (!confirm || !confirm(prompt)) {
            res.error = QStringLiteral("committed, not pushed");
            return res;
        }
    }

    const GitResult p = git({QStringLiteral("push")});
    if (!p.ok()) {
        res.error = QStringLiteral("git push failed: %1").arg(p.output.trimmed());
        return res;
    }
    res.pushed = true;
    return res;
}

bool GitFlow::prText(const QString& commits, const QString& diff, const QString& backendId,
                     const QString& model, QString* title, QString* body,
                     const CancelToken& cancel) {
    const QString sys = QStringLiteral(
        "Write a pull-request title and body. Output ONLY JSON: {\"title\":\"...\",\"body\":\"...\"}. "
        "Title is a concise imperative summary (<=72 chars). Body is short markdown: a one-line "
        "summary, then a \"## Changes\" bullet list, then \"## Testing\" if evident. No fluff.");
    const QString user =
        QStringLiteral("Commits:\n%1\n\nDiff:\n%2").arg(commits, diff.left(kDiffBudget));
    const QJsonObject j = askJson(backendId, model, sys, user, cancel);

    const QString t = j.value(QStringLiteral("title")).toString().trimmed();
    const QString b = j.value(QStringLiteral("body")).toString().trimmed();
    if (title) *title = t.isEmpty() ? QStringLiteral("Update") : t;
    // Falling back to the raw commit list beats opening a PR with an empty body.
    if (body) *body = b.isEmpty() ? commits : b;
    return !t.isEmpty();
}

PrReview GitFlow::review(const QString& diff, const QString& backendId, const QString& model,
                         const CancelToken& cancel) {
    const QString sys = QStringLiteral(
        "You are a meticulous code reviewer. Review the diff for correctness, security (injection, "
        "unsafe shell, secrets), and whether it stays in scope. Output ONLY JSON: "
        "{\"verdict\":\"approve|comment|request_changes\",\"summary\":\"one line\","
        "\"findings\":[\"file:line — issue\", ...]}. Be concrete; no nitpicks unless they matter.");
    const QJsonObject j = askJson(backendId, model, sys,
                                  QStringLiteral("Diff:\n") + diff.left(kReviewBudget), cancel);

    PrReview r;
    if (j.isEmpty()) return r;  // verdict stays empty: the caller reports "unavailable"
    r.verdict = j.value(QStringLiteral("verdict")).toString(QStringLiteral("comment"));
    r.summary = j.value(QStringLiteral("summary")).toString();
    for (const QJsonValue& v : j.value(QStringLiteral("findings")).toArray())
        r.findings << v.toString();
    return r;
}

}  // namespace odv
