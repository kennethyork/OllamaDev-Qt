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

    out() << "\n👁  OllamaDev Watch  " << dim(QStringLiteral("model %1 · %2 · %3 mode")
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

    QHash<QString, qint64> prev = snapshot(roots);

    // inotify costs a descriptor per DIRECTORY and the kernel caps it
    // (fs.inotify.max_user_watches, 8192 by default on many distros). A monorepo
    // blows through that, and QFileSystemWatcher's only signal is the list of paths
    // it could NOT take — so watch that list and degrade to polling rather than
    // silently missing changes in whatever it dropped.
    QStringList dirs;
    snapshot(roots, &dirs);

    QFileSystemWatcher watcher;
    const QStringList rejected = watcher.addPaths(dirs);
    const bool polling = !rejected.isEmpty();
    if (polling) {
        watcher.removePaths(watcher.directories());
        out() << dim(QStringLiteral("watching %1 dirs by polling every %2s (the kernel would "
                                    "not take %3 more inotify watches)")
                         .arg(dirs.size())
                         .arg(interval)
                         .arg(rejected.size()))
              << "\n";
    } else {
        out() << dim(QStringLiteral("watching %1 dirs · Ctrl-C to stop").arg(dirs.size())) << "\n";
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
        prev = snapshot(roots);

        // A new directory is not watched until we add it (inotify is per-dir, and
        // an unwatched dir means its files are invisible from here on).
        if (!polling) {
            QStringList fresh;
            snapshot(roots, &fresh);
            const QStringList have = watcher.directories();
            QStringList add;
            for (const QString& d : fresh)
                if (!have.contains(d)) add << d;
            if (!add.isEmpty()) watcher.addPaths(add);
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
