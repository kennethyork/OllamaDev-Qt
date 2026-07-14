// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Watch.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHash>
#include <QQueue>
#include <QSet>
#include <QTextStream>
#include <QTimer>

#include "Agent.h"
#include "Backend.h"
#include "Config.h"
#include "Tools.h"
#include "Tui.h"

namespace odv {
namespace {

// Source extensions worth reacting to. Binaries, lockfiles and media change all
// the time (a build writes them) and re-running an agent on them is pure waste.
const QSet<QString>& sourceExts() {
    static const QSet<QString> e{
        QStringLiteral("php"),  QStringLiteral("js"),   QStringLiteral("ts"),
        QStringLiteral("jsx"),  QStringLiteral("tsx"),  QStringLiteral("py"),
        QStringLiteral("go"),   QStringLiteral("rs"),   QStringLiteral("rb"),
        QStringLiteral("java"), QStringLiteral("c"),    QStringLiteral("h"),
        QStringLiteral("cpp"),  QStringLiteral("hpp"),  QStringLiteral("cc"),
        QStringLiteral("cs"),   QStringLiteral("html"), QStringLiteral("css"),
        QStringLiteral("json"), QStringLiteral("yml"),  QStringLiteral("yaml"),
        QStringLiteral("md"),   QStringLiteral("sh"),   QStringLiteral("sql"),
        QStringLiteral("vue"),  QStringLiteral("svelte")};
    return e;
}

const QSet<QString>& skipDirs() {
    static const QSet<QString> d{QStringLiteral(".git"),      QStringLiteral(".ollamadev"),
                                 QStringLiteral("node_modules"), QStringLiteral("vendor"),
                                 QStringLiteral("dist"),      QStringLiteral("build"),
                                 QStringLiteral(".build"),    QStringLiteral(".svn"),
                                 QStringLiteral("__pycache__")};
    return d;
}

bool interesting(const QFileInfo& fi) {
    return sourceExts().contains(fi.suffix().toLower());
}

// One walk answers both questions: which files to diff, and which directories to
// hand to inotify.
void walk(const QString& root, QHash<QString, qint64>* files, QStringList* dirs) {
    const QFileInfo rootInfo(root);
    if (rootInfo.isFile()) {
        if (files) files->insert(rootInfo.absoluteFilePath(), rootInfo.lastModified().toMSecsSinceEpoch());
        return;
    }
    if (!rootInfo.isDir()) return;

    QQueue<QString> todo;
    todo.enqueue(rootInfo.absoluteFilePath());
    while (!todo.isEmpty()) {
        const QString cur = todo.dequeue();
        if (dirs) dirs->append(cur);
        QDir d(cur);
        for (const QFileInfo& fi : d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot |
                                                   QDir::NoSymLinks)) {
            if (fi.isDir()) {
                if (skipDirs().contains(fi.fileName())) continue;
                todo.enqueue(fi.absoluteFilePath());
                continue;
            }
            if (files && interesting(fi))
                files->insert(fi.absoluteFilePath(), fi.lastModified().toMSecsSinceEpoch());
        }
    }
}

QHash<QString, qint64> snapshot(const QStringList& roots, QStringList* dirs = nullptr) {
    QHash<QString, qint64> snap;
    for (const QString& r : roots) walk(r, &snap, dirs);
    return snap;
}

// New files, and files whose mtime advanced. A deletion is not work to do.
QStringList changedSince(const QHash<QString, qint64>& before, const QHash<QString, qint64>& now) {
    QStringList out;
    for (auto it = now.constBegin(); it != now.constEnd(); ++it) {
        const auto prev = before.constFind(it.key());
        if (prev == before.constEnd() || prev.value() < it.value()) out << it.key();
    }
    out.sort();
    return out;
}

QString relativeTo(const QString& path, const QStringList& roots) {
    for (const QString& r : roots) {
        const QString root = QFileInfo(r).isDir()
                                 ? QDir(r).absolutePath() + QLatin1Char('/')
                                 : QFileInfo(r).absoluteFilePath();
        if (path.startsWith(root)) return path.mid(root.size());
    }
    const QString cwd = QDir::currentPath() + QLatin1Char('/');
    return path.startsWith(cwd) ? path.mid(cwd.size()) : path;
}

QTextStream& out() {
    static QTextStream s(stdout);
    return s;
}

QString dim(const QString& s) {
    return Render::enabled() ? QString::fromUtf8(ansi::kDim) + s + QString::fromUtf8(ansi::kReset)
                             : s;
}

// One bounded agent pass over the task, in the ambient permission mode: --auto
// applies fixes, ask prompts, readonly just reports.
void act(Agent& agent, const QString& task, const QStringList& changed, int maxIterations) {
    QString prompt = QStringLiteral("Files just changed: %1\n\n").arg(changed.join(QStringLiteral(", ")));
    prompt += QStringLiteral("Do the following, using your tools where needed, then stop:\n") + task;

    QVector<ChatMessage> msgs{
        {QStringLiteral("system"), agent.buildSystemPrompt(QDir::currentPath()), {}, {}, {}, {}, {}},
        {QStringLiteral("user"), prompt, {}, {}, {}, {}, {}}};

    StreamSink sink;
    sink.onContent = [](const QString& c) {
        out() << dim(c);
        out().flush();
    };

    CancelToken cancel;
    agent.loop(msgs, maxIterations, sink, cancel);
    out() << "\n";
    out().flush();
}

}  // namespace

int Watch::run(const WatchOptions& opt) {
    const QString task = opt.task.trimmed();
    if (task.isEmpty()) {
        out() << "Usage: ollamadev watch \"<task>\" [paths…] [--interval N] [--once]\n";
        out().flush();
        return 2;
    }

    QStringList roots;
    for (const QString& p : opt.paths) {
        const QFileInfo fi(p);
        if (!fi.exists()) {
            out() << "no such path: " << p << "\n";
            out().flush();
            return 2;
        }
        roots << fi.absoluteFilePath();
    }
    if (roots.isEmpty()) roots << QDir::currentPath();

    const QString backendId =
        opt.backend.isEmpty() ? Config::str(QStringLiteral("model.backend"), QStringLiteral("ollama"))
                              : opt.backend;
    auto backend = Backends::get(backendId);
    if (!backend || !backend->available()) {
        out() << "cannot reach the " << backendId << " backend — start it first (ollama serve).\n";
        out().flush();
        return 1;
    }
    const QString model = opt.model.isEmpty() ? backend->defaultModel() : opt.model;

    Tools::registerAll();
    Tools::setThreadRoot(QDir::currentPath());
    Permission::setInteractive(Tui::stdinIsTty());
    Permission::setMode(Permission::modeFromName(
        Config::str(QStringLiteral("permissions.mode"), QStringLiteral("ask"))));

    Agent agent(backendId, model);
    const int interval = qMax(1, opt.intervalSec);
    const int iterations = qMax(2, opt.iterations);

    QStringList names;
    for (const QString& r : roots) names << QFileInfo(r).fileName();

    out() << "\n OllamaDev Watch  " << dim(QStringLiteral("model %1 · %2 · %3 mode")
                                                 .arg(model, names.join(QStringLiteral(", ")),
                                                      Permission::modeName(Permission::mode())))
          << "\n"
          << dim(QStringLiteral("Task: ") + task) << "\n";
    out().flush();

    // --once runs the task NOW and exits. The PHP made --once wait for a change
    // first, which meant the flag could never be scripted or smoke-tested: with a
    // quiet tree it simply hung forever. "Once" here means once.
    if (opt.once) {
        QStringList changed;
        for (const QString& f : snapshot(roots).keys()) changed << relativeTo(f, roots);
        changed.sort();
        act(agent, task, changed, iterations);
        return 0;
    }

    QStringList dirs;
    QHash<QString, qint64> prev = snapshot(roots, &dirs);
    dirs.removeDuplicates();  // overlapping roots ("watch . src") must not double-add

    // WATCH THE FILES, NOT JUST THE DIRECTORIES. QFileSystemWatcher's
    // directoryChanged only fires when an entry is ADDED, REMOVED or RENAMED —
    // editing an existing file in place (`>>`, an in-place save, a compiler
    // rewriting an object) changes nothing about the directory and emits NOTHING.
    // The file itself has to be watched to hear that, so the watch set is
    // dirs (new/deleted files) + source files (edits).
    const auto watchSet = [&](const QHash<QString, qint64>& files, const QStringList& ds) {
        QStringList all = ds;
        all += files.keys();
        all.removeDuplicates();
        return all;
    };

    // inotify costs one watch per PATH, and that budget is PER USER and SHARED
    // (fs.inotify.max_user_watches) — it is not ours to drain. A watcher that grabs
    // one for every path in a monorepo does two bad things: it hits the cap and
    // then silently misses changes in whatever it could not add, and it starves the
    // user's editor of watches while it runs. So cap what we ask for, and fall back
    // to polling the moment either the cap or the kernel says no. Polling is
    // slower; it is never wrong.
    const int cap = qMax(64, Config::integer(QStringLiteral("watch.maxWatches"), 4096));
    const QStringList initial = watchSet(prev, dirs);

    QFileSystemWatcher watcher;
    QStringList rejected;
    bool polling = initial.size() > cap;
    if (!polling) {
        rejected = watcher.addPaths(initial);
        polling = !rejected.isEmpty();
    }

    if (polling) {
        // Hand every watch back: half-watching a tree is worse than not watching it,
        // because the changes it misses are invisible rather than merely late.
        const QStringList held = watcher.directories() + watcher.files();
        if (!held.isEmpty()) watcher.removePaths(held);
        const QString why =
            initial.size() > cap
                ? QStringLiteral("%1 paths is over the %2-watch cap").arg(initial.size()).arg(cap)
                : QStringLiteral("the kernel refused %1 of %2 inotify watches")
                      .arg(rejected.size())
                      .arg(initial.size());
        out() << dim(QStringLiteral("polling every %1s — %2 · Ctrl-C to stop").arg(interval).arg(why))
              << "\n";
    } else {
        out() << dim(QStringLiteral("watching %1 files in %2 dirs · Ctrl-C to stop")
                         .arg(prev.size())
                         .arg(dirs.size()))
              << "\n";
    }
    out().flush();

    QEventLoop loop;

    // Debounce: an editor's save is often several inotify events (write, chmod,
    // rename-over), and a build writes hundreds. Collapse a burst into one run.
    QTimer debounce;
    debounce.setSingleShot(true);
    debounce.setInterval(interval * 1000);

    const auto fire = [&] {
        const QHash<QString, qint64> now = snapshot(roots);
        const QStringList changed = changedSince(prev, now);
        prev = now;
        if (changed.isEmpty()) return;

        QStringList shown;
        for (const QString& f : changed.mid(0, 12)) shown << relativeTo(f, roots);
        out() << "\n▸ change "
              << dim(QStringLiteral("%1 · %2 file(s): %3%4")
                         .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")))
                         .arg(changed.size())
                         .arg(shown.join(QStringLiteral(", ")),
                              changed.size() > 12 ? QStringLiteral("…") : QString()))
              << "\n";
        out().flush();

        QStringList rel;
        for (const QString& f : changed) rel << relativeTo(f, roots);
        act(agent, task, rel, iterations);

        // The agent just edited files. Re-snapshot so its OWN writes do not look
        // like a change and retrigger it — that loop never ends.
        QStringList freshDirs;
        prev = snapshot(roots, &freshDirs);

        // Re-arm. Two reasons this is not optional:
        //   * a file or directory created since we started is not watched, and an
        //     unwatched path is invisible from here on;
        //   * an editor that saves by writing a temp file and RENAMING it over the
        //     original replaces the inode, and QFileSystemWatcher drops a watch whose
        //     file was renamed away — so the second save of a file would be missed.
        if (!polling) {
            const QStringList have = watcher.directories() + watcher.files();
            QStringList add;
            for (const QString& p : watchSet(prev, freshDirs))
                if (!have.contains(p)) add << p;
            if (!add.isEmpty() && have.size() + add.size() <= cap) watcher.addPaths(add);
        }
    };

    QObject::connect(&debounce, &QTimer::timeout, &loop, fire);
    QObject::connect(&watcher, &QFileSystemWatcher::directoryChanged, &loop,
                     [&debounce](const QString&) { debounce.start(); });
    QObject::connect(&watcher, &QFileSystemWatcher::fileChanged, &loop,
                     [&debounce](const QString&) { debounce.start(); });

    QTimer poll;
    if (polling) {
        QObject::connect(&poll, &QTimer::timeout, &loop, fire);
        poll.start(interval * 1000);
    }

    return loop.exec();
}

}  // namespace odv
