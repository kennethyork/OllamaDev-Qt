#include "Crew.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTextStream>
#include <QThread>
#include <QUuid>

#include "Agent.h"
#include "Board.h"
#include "Config.h"
#include "Json.h"
#include "Models.h"
#include "Parallel.h"
#include "SecScan.h"
#include "Tools.h"

namespace odv {

namespace {

QString newRunId() {
    return QStringLiteral("crew_%1").arg(QDateTime::currentSecsSinceEpoch());
}

QString runDir(const QString& runId) {
    return Config::crewDir() + "/" + runId;
}

// Admission key for the limiter. Two coders on the same local Ollama contend
// for the same GPU slots; a coder on Claude Code and a coder on a :cloud model
// contend with nobody. Keying on backend + locality is what lets a mixed crew
// throttle each provider independently instead of all-or-nothing.
QString limiterKey(const QString& backendId, const QString& model) {
    if (backendId == "ollama") {
        return Models::isCloud(model) ? QStringLiteral("ollama:cloud")
                                      : QStringLiteral("ollama:local");
    }
    return backendId;
}

QString pick(const QStringList& list, int i, const QString& fallback) {
    if (list.isEmpty()) return fallback;
    return list.at(i % list.size());
}

void writeFile(const QString& path, const QString& text) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(text.toUtf8());
}

// The live kanban the GUI polls.
void publishBoard(const QString& runId, const QString& task, const QVector<Subtask>& subs,
                  bool active) {
    QJsonArray arr;
    for (const auto& s : subs) {
        arr.append(QJsonObject{{"n", s.n},
                               {"title", s.title},
                               {"role", s.role},
                               {"state", s.state},
                               {"backend", s.backend},
                               {"model", s.model}});
    }
    QJsonObject o{{"runId", runId},
                  {"task", task},
                  {"active", active},
                  {"subtasks", arr},
                  {"ts", QDateTime::currentMSecsSinceEpoch()}};
    writeFile(Config::crewDir() + "/current.json", QString::fromUtf8(json::encode(o)));
}

}  // namespace

Crew::Result Crew::run(const CrewOptions& opts, const CrewEvents& ev, const CancelToken& cancel) {
    Result out;
    const QString projectRoot = QDir::currentPath();
    const QString runId = newRunId();
    out.runId = runId;

    const QString sessionBackend = Config::str("model.backend", "ollama");
    const QString sessionModel = Config::str("ollama.defaultModel", "");

    auto phase = [&](const QString& p, const QString& m) {
        if (ev.onPhase) ev.onPhase(p, m);
    };
    auto backendFor = [&](const QString& want) {
        return want.isEmpty() ? sessionBackend : want;
    };

    // ---- Researcher (read-only, shared context for everyone downstream) ----
    QString research;
    if (opts.research && !cancel.cancelled()) {
        phase("research", "investigating the codebase");
        Agent r(backendFor(opts.researcherBackend),
                opts.researcherModel.isEmpty() ? sessionModel : opts.researcherModel);
        Permission::setMode(PermMode::ReadOnly);
        Permission::setInteractive(false);
        QVector<ChatMessage> msgs{
            {"system", "You are the Researcher. Investigate the codebase using READ-ONLY tools "
                       "(ls, grep, view, glob). Report what a team of coders needs to know: where "
                       "things live, the conventions in use, and the files they will need to "
                       "touch. Do NOT edit anything.",
             {}, {}, {}, {}, {}},
            {"user", "Task: " + opts.task, {}, {}, {}, {}, {}}};
        research = r.loop(msgs, Config::integer("crew.researchIterations", 6), {}, cancel);
        writeFile(runDir(runId) + "/research.md", research);
    }

    // ---- Director: decompose into independent, non-overlapping subtasks ----
    phase("plan", "director is decomposing the task");
    const int maxCoders = qBound(1, opts.maxCoders, 8);
    QVector<Subtask> subs;
    {
        Agent d(backendFor(opts.directorBackend),
                opts.directorModel.isEmpty() ? sessionModel : opts.directorModel);
        QString sys = QStringLiteral(
            "You are the Director. Decompose the task into at most %1 INDEPENDENT subtasks that, "
            "wherever possible, touch DIFFERENT files so they can be built in parallel without "
            "conflicting. Reply with JSON only: "
            "{\"subtasks\":[{\"title\":\"...\",\"role\":\"coder\",\"prompt\":\"...\"}]}")
                          .arg(maxCoders);
        QString usr = "Task: " + opts.task;
        if (!opts.focus.isEmpty()) usr += "\nFocus: " + opts.focus;
        if (!research.isEmpty()) usr += "\n\nResearch findings:\n" + research.left(6000);

        auto backend = Backends::get(d.backendId());
        QJsonObject planned =
            backend ? backend->chatJson(d.model(),
                                        {{"system", sys, {}, {}, {}, {}, {}},
                                         {"user", usr, {}, {}, {}, {}, {}}},
                                        cancel)
                    : QJsonObject{};

        int n = 0;
        for (const auto& v : planned.value("subtasks").toArray()) {
            if (n >= maxCoders) break;
            const auto o = v.toObject();
            Subtask s;
            s.n = ++n;
            s.title = o.value("title").toString();
            s.role = o.value("role").toString("coder");
            s.prompt = o.value("prompt").toString(s.title);
            s.state = "todo";
            s.backend = backendFor(pick(opts.coderBackends, s.n - 1, opts.coderBackend));
            s.model = pick(opts.coderModels, s.n - 1,
                           opts.coderModel.isEmpty() ? sessionModel : opts.coderModel);
            if (!s.title.isEmpty()) subs.append(s);
        }
    }
    if (subs.isEmpty()) {
        phase("error", "the director produced no subtasks");
        return out;
    }
    out.subtasks = subs;
    publishBoard(runId, opts.task, subs, true);

    // ---- Admission control ------------------------------------------------
    // Ask every backend how wide it can actually go, and cap each key at the
    // MINIMUM the participating models allow. A local 9b model on one GPU has
    // ~2 real slots; going wider just queues inside Ollama while each extra
    // slot costs a full KV cache. A :cloud model or a coding CLI has no such
    // constraint, so those keys open up.
    {
        QHash<QString, int> caps;
        for (const auto& s : subs) {
            auto b = Backends::get(s.backend);
            if (!b) continue;
            const QString key = limiterKey(s.backend, s.model);
            const int lim = qMax(1, b->concurrencyLimit(s.model));
            caps[key] = caps.contains(key) ? qMin(caps[key], lim) : lim;
        }
        for (auto it = caps.constBegin(); it != caps.constEnd(); ++it) {
            int lim = it.value();
            if (opts.parallel > 0) lim = qMin(lim, opts.parallel);
            Limiter::instance().setLimit(it.key(), lim);
            if (ev.onLog)
                ev.onLog(QStringLiteral("%1: up to %2 concurrent").arg(it.key()).arg(lim));
        }
    }

    // ---- Sandboxes: one full copy of the project per coder -----------------
    phase("sandbox", QStringLiteral("preparing %1 sandboxes").arg(subs.size()));
    const QString wtRoot = QDir::tempPath() + "/ollamadev-crew/" + runId;
    const QString csRoot = Config::dataDir() + "/crew/" + runId + "/changeset";
    QVector<QString> sandboxes(subs.size());
    QVector<QString> stores(subs.size());
    {
        QVector<int> idx(subs.size());
        for (int i = 0; i < subs.size(); ++i) idx[i] = i;
        // Copying is pure I/O, so it fans out freely — this key has no model behind it.
        Limiter::instance().setLimit("io", QThread::idealThreadCount());
        parallelRun(
            subs.size(), [](int) { return QStringLiteral("io"); },
            [&](int i) -> QJsonObject {
                sandboxes[i] = wtRoot + QStringLiteral("/c%1").arg(i + 1);
                stores[i] = csRoot + QStringLiteral("/c%1").arg(i + 1);
                QString err;
                Sandbox::copyTree(projectRoot, sandboxes[i], &err);
                return QJsonObject{{"err", err}};
            });
    }

    // ---- Coders, genuinely in parallel ------------------------------------
    phase("build", QStringLiteral("%1 coders working").arg(subs.size()));
    QVector<CoderResult> results(subs.size());

    parallelRun(
        subs.size(),
        [&](int i) { return limiterKey(subs[i].backend, subs[i].model); },
        [&](int i) -> QJsonObject {
            const Subtask& st = subs[i];
            if (cancel.cancelled()) return {};

            subs[i].state = "doing";
            if (ev.onCoderState) ev.onCoderState(st.n, "doing");
            publishBoard(runId, opts.task, subs, true);

            const QString log = runDir(runId) + QStringLiteral("/coder-%1.log").arg(st.n);

            // Each coder is chrooted-by-convention into its own copy: the tools
            // resolve relative paths against the sandbox, so two coders editing
            // "the same" file never see each other.
            Agent a(st.backend, st.model);
            Permission::setMode(PermMode::Auto);
            Permission::setInteractive(false);

            QString sys = a.buildSystemPrompt(sandboxes[i]);
            sys += "\n\nYou are Coder #" + QString::number(st.n) +
                   ". Work ONLY inside your assigned subtask. Make the edits directly.";
            QString usr = "Overall goal: " + opts.task + "\n\nYour subtask: " + st.title + "\n" +
                          st.prompt;
            if (!research.isEmpty()) usr += "\n\nShared research:\n" + research.left(4000);

            QVector<ChatMessage> msgs{{"system", sys, {}, {}, {}, {}, {}},
                                      {"user", usr, {}, {}, {}, {}, {}}};

            StreamSink sink;
            sink.onContent = [&, n = st.n](const QString& chunk) {
                if (ev.onCoderOutput) ev.onCoderOutput(n, chunk);
                QFile f(log);
                if (f.open(QIODevice::Append)) f.write(chunk.toUtf8());
            };

            // Thread-local, NOT QDir::setCurrent — cwd is process-wide and
            // parallel coders would stomp each other into the wrong sandbox.
            Tools::setThreadRoot(sandboxes[i]);
            a.loop(msgs, Config::integer("crew.coderIterations", 10), sink, cancel);

            CoderResult r;
            r.n = st.n;
            const Changeset cs = Sandbox::capture(projectRoot, sandboxes[i], stores[i]);
            r.empty = cs.empty();
            r.store = cs.store;
            r.files = cs.files();
            r.diff = cs.diff;
            results[i] = r;

            subs[i].state = r.empty ? "held" : "done";
            if (ev.onCoderState) ev.onCoderState(st.n, subs[i].state);
            publishBoard(runId, opts.task, subs, true);
            return {};
        });

    // ---- Auditors, also in parallel ---------------------------------------
    if (opts.audit && !cancel.cancelled()) {
        phase("audit", "reviewing every changeset");
        const QString ab = backendFor(opts.auditorBackend);
        const QString am = opts.auditorModel.isEmpty() ? sessionModel : opts.auditorModel;
        parallelRun(
            results.size(), [&](int) { return limiterKey(ab, am); },
            [&](int i) -> QJsonObject {
                if (results[i].empty || cancel.cancelled()) return {};
                auto backend = Backends::get(ab);
                if (!backend) return {};
                const QString sys =
                    "You are the Auditor. Review this changeset for correctness, security, and "
                    "scope creep. Mark it unclean if it is wrong, unsafe, or does work outside "
                    "the stated subtask. Reply with JSON only: "
                    "{\"clean\":true|false,\"summary\":\"one line\",\"issues\":[\"...\"]}";
                const QString usr = "Subtask: " + subs[i].title + "\n\nDiff:\n" +
                                    results[i].diff.left(16000);
                const QJsonObject v = backend->chatJson(
                    am, {{"system", sys, {}, {}, {}, {}, {}}, {"user", usr, {}, {}, {}, {}, {}}},
                    cancel);
                results[i].audit.clean = v.value("clean").toBool(false);
                results[i].audit.summary = v.value("summary").toString();
                for (const auto& is : v.value("issues").toArray())
                    results[i].audit.issues << is.toString();
                return {};
            });
    } else {
        for (auto& r : results) r.audit.clean = true;
    }

    // ---- Landing ----------------------------------------------------------
    // Sequential and deterministic (coder order), because this is the only
    // place that writes to the user's tree and first-writer-wins must be
    // reproducible.
    phase("land", "applying clean work");
    const bool reviewMode = (opts.land == "review");
    QHash<QString, int> touched;  // file -> the coder that already claimed it

    auto hold = [&](const CoderResult& r, const QString& reason) {
        out.held.append(r.n);
        subs[r.n - 1].state = "held";
        Decision d;
        d.kind = "crew_branch";
        d.summary = QStringLiteral("coder #%1 — %2").arg(r.n).arg(subs[r.n - 1].title);
        d.detail = r.diff.left(60000);
        d.data = QJsonObject{{"runId", runId},
                             {"n", r.n},
                             {"repoRoot", projectRoot},
                             {"store", r.store},
                             {"reason", reason},
                             {"files", QJsonArray::fromStringList(r.files)}};
        Board::enqueue(d);
        if (ev.onLog) ev.onLog(QStringLiteral("held #%1 — %2").arg(r.n).arg(reason));
    };

    for (auto& r : results) {
        if (r.n == 0 || r.empty) continue;

        // A changeset that introduces a credential is NEVER auto-applied, no
        // matter how clean the audit was.
        QVector<Finding> high;
        for (const auto& f : SecScan::scanDiff(r.diff))
            if (f.severity == "high") high.append(f);
        if (!high.isEmpty()) {
            hold(r, QStringLiteral("secret detected (%1) — not auto-applied").arg(high.size()));
            continue;
        }
        if (reviewMode) {
            hold(r, "review mode");
            continue;
        }
        if (!r.audit.clean) {
            hold(r, "audit flagged: " + r.audit.summary);
            continue;
        }

        // No merge, so two coders touching the same path cannot both land.
        // First writer wins; the loser is held with its diff intact.
        QString clash;
        for (const auto& f : r.files) {
            if (touched.contains(f)) {
                clash = f;
                break;
            }
        }
        if (!clash.isEmpty()) {
            hold(r, QStringLiteral("overlaps coder #%1 on %2").arg(touched[clash]).arg(clash));
            continue;
        }

        QStringList wrote;
        QString err;
        if (!Sandbox::apply(r.store, projectRoot, &wrote, &err)) {
            hold(r, "could not write files: " + err);
            continue;
        }
        for (const auto& f : r.files) touched.insert(f, r.n);
        out.applied.append(r.n);
        subs[r.n - 1].state = "done";
        if (ev.onLog)
            ev.onLog(QStringLiteral("applied #%1 (%2 files)").arg(r.n).arg(wrote.size()));
    }

    out.results = results;
    publishBoard(runId, opts.task, subs, false);
    for (const auto& sb : sandboxes) Sandbox::removeTree(sb);
    phase("done", QStringLiteral("%1 applied · %2 held")
                      .arg(out.applied.size())
                      .arg(out.held.size()));
    return out;
}

bool Crew::accept(int n, QString* err) {
    for (const auto& d : Board::pending()) {
        if (d.kind != "crew_branch" || d.data.value("n").toInt() != n) continue;
        QStringList wrote;
        if (!Sandbox::apply(d.data.value("store").toString(),
                            d.data.value("repoRoot").toString(), &wrote, err))
            return false;
        Board::decide(d.id, "accept");
        return true;
    }
    if (err) *err = QStringLiteral("no held work numbered %1").arg(n);
    return false;
}

bool Crew::discard(int n, QString* err) {
    for (const auto& d : Board::pending()) {
        if (d.kind != "crew_branch" || d.data.value("n").toInt() != n) continue;
        Sandbox::removeTree(d.data.value("store").toString());
        Board::decide(d.id, "deny");
        return true;
    }
    if (err) *err = QStringLiteral("no held work numbered %1").arg(n);
    return false;
}

bool Crew::steer(int coder, const QString& message, QString* err) {
    const QJsonObject b = boardState();
    const QString runId = b.value("runId").toString();
    if (runId.isEmpty()) {
        if (err) *err = "no crew is running";
        return false;
    }
    QFile f(runDir(runId) + "/steer.jsonl");
    QDir().mkpath(runDir(runId));
    if (!f.open(QIODevice::Append)) {
        if (err) *err = "could not write steer.jsonl";
        return false;
    }
    const QJsonObject line{{"target", coder},
                           {"msg", message},
                           {"ts", QDateTime::currentMSecsSinceEpoch()}};
    f.write(json::encode(line) + "\n");
    return true;
}

QJsonObject Crew::boardState() {
    QFile f(Config::crewDir() + "/current.json");
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

void Crew::clearBoard() {
    QFile::remove(Config::crewDir() + "/current.json");
    Board::clear();
}

}  // namespace odv
