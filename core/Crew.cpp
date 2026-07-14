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

#include <algorithm>

#include "Agent.h"
#include "Board.h"
#include "Config.h"
#include "Json.h"
#include "Memory.h"
#include "Models.h"
#include "Parallel.h"
#include "Router.h"
#include "SecScan.h"
#include "Skills.h"
#include "Tools.h"
#include "Usage.h"

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
                               {"model", s.model},
                               {"route", s.route}});
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
    if (opts.security) return securityScan(opts, ev, cancel);

    Result out;
    const QString projectRoot = QDir::currentPath();
    const QString runId = newRunId();
    out.runId = runId;

    const QString sessionBackend = Config::str("model.backend", "ollama");
    const QString sessionModel = Config::str("ollama.defaultModel", "");

    // Token accounting baseline — the report at the end is this run's delta.
    const QMap<QString, Usage::Tally> usageBefore = Usage::snapshot();

    auto phase = [&](const QString& p, const QString& m) {
        if (ev.onPhase) ev.onPhase(p, m);
    };
    auto backendFor = [&](const QString& want) {
        return want.isEmpty() ? sessionBackend : want;
    };
    // A model tag belongs to exactly one backend, so the session default may only
    // be inherited by a role that is actually ON the session's backend. Give an
    // Ollama tag to a Claude or Codex role and that CLI rejects the whole run.
    auto modelFor = [&](const QString& backendId, const QString& want) -> QString {
        if (!want.isEmpty()) return want;
        if (backendId == sessionBackend) return sessionModel;
        auto b = Backends::get(backendId);
        return b ? b->defaultModel() : QString();
    };
    // The routing brain for the fixed-tier roles. An explicit model still wins;
    // otherwise, with --route on, an Ollama role gets the model for its tier.
    auto routedForTier = [&](const QString& backendId, const QString& want,
                             const QString& tier) -> QString {
        if (!want.isEmpty()) return want;
        if (opts.route && backendId == QLatin1String("ollama")) return Router::modelForTier(tier);
        return modelFor(backendId, want);
    };

    // ---- Researcher (read-only, shared context for everyone downstream) ----
    QString research;
    if (opts.research && !cancel.cancelled()) {
        phase("research", "investigating the codebase");
        const QString rb = backendFor(opts.researcherBackend);
        Agent r(rb, routedForTier(rb, opts.researcherModel, QStringLiteral("moderate")));
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

    // ---- Prior knowledge (learning loop, opt-in) --------------------------
    // What earlier runs distilled — memory facts and the skills catalog — folded
    // into the shared context so this run starts already knowing them. Reuses the
    // `research` channel that every downstream role already receives.
    if (opts.learn) {
        QString prior;
        const QString mem = Memory::index(24);
        if (!mem.isEmpty()) prior += "What past runs learned about this project:\n" + mem + "\n\n";
        const QString skills = Skills::catalogFor(projectRoot);
        if (!skills.isEmpty()) prior += "Skills available:\n" + skills + "\n";
        if (!prior.isEmpty()) {
            research = prior + (research.isEmpty() ? QString() : "\n---\n" + research);
            if (ev.onLog) ev.onLog("loaded prior knowledge (memory + skills)");
        }
    }

    // ---- Director: decompose into independent, non-overlapping subtasks ----
    phase("plan", "director is decomposing the task");
    // Swarm mode raises the ceiling; the per-backend limiter still throttles real
    // concurrency, so a big number just means more queued work, not more GPU load.
    const int cap = opts.swarmMax > 0 ? qMin(opts.swarmMax, 64) : 8;
    const int maxCoders = qBound(1, opts.maxCoders, cap);
    QVector<Subtask> subs;
    {
        const QString db = backendFor(opts.directorBackend);
        Agent d(db, routedForTier(db, opts.directorModel, QStringLiteral("hard")));
        QString sys = QStringLiteral(
            "You are the Director. Decompose the task into at most %1 INDEPENDENT subtasks that, "
            "wherever possible, touch DIFFERENT files so they can be built in parallel without "
            "conflicting. Reply with JSON only: "
            "{\"subtasks\":[{\"title\":\"...\",\"role\":\"coder\",\"prompt\":\"...\"}]}")
                          .arg(maxCoders);
        // The Director can only assign a role it knows exists; an invented one
        // falls back to 'coder' in CrewRoles::get().
        sys += "\n\nAssign each subtask the most fitting role:\n" + CrewRoles::catalog();
        QString usr = "Task: " + opts.task;
        if (!opts.focus.isEmpty()) usr += "\nFocus: " + opts.focus;
        if (!research.isEmpty()) usr += "\n\nResearch findings:\n" + research.left(6000);

        auto backend = Backends::get(d.backendId());

        // Models do not reliably honour the schema on the first try — a cloud
        // model in particular will sometimes answer in prose. One firm retry
        // costs a few seconds and turns an intermittent "no subtasks" failure
        // into a non-event.
        QJsonArray planned;
        for (int attempt = 0; attempt < 2 && backend && !cancel.cancelled(); ++attempt) {
            QString ask = usr;
            if (attempt > 0)
                ask += "\n\nYour previous reply could not be parsed. Reply with NOTHING but the "
                       "JSON object described above.";
            const QJsonObject o =
                backend->chatJson(d.model(),
                                  {{"system", sys, {}, {}, {}, {}, {}},
                                   {"user", ask, {}, {}, {}, {}, {}}},
                                  cancel);
            planned = o.value("subtasks").toArray();
            // Tolerate a bare array, or a single object, the way the PHP planner did.
            if (planned.isEmpty() && o.contains("title")) planned.append(o);
            if (!planned.isEmpty()) break;
            if (attempt == 0 && ev.onLog) ev.onLog("director reply did not parse — retrying");
        }

        int n = 0;
        for (const auto& v : planned) {
            if (n >= maxCoders) break;
            const auto o = v.toObject();
            Subtask s;
            s.n = ++n;
            s.title = o.value("title").toString();
            s.role = o.value("role").toString("coder");
            s.prompt = o.value("prompt").toString(s.title);
            s.state = "todo";
            s.backend = backendFor(pick(opts.coderBackends, s.n - 1, opts.coderBackend));

            // A model tag belongs to exactly ONE backend. The session default is
            // an Ollama tag, so handing it to a Claude or Codex coder makes that
            // CLI reject the run outright ("model may not exist"). Only inherit
            // the session model when the coder is on the session's backend;
            // otherwise ask that backend for its own default.
            s.model = pick(opts.coderModels, s.n - 1, opts.coderModel);
            if (s.model.isEmpty()) {
                const QString pinned = CrewRoles::get(s.role).model;
                if (!pinned.isEmpty()) {
                    s.model = pinned;
                } else if (opts.route && s.backend == QLatin1String("ollama")) {
                    // Route each coder on its OWN subtask — a "rename a variable"
                    // subtask and a "design the parser" subtask should not get the
                    // same model just because they are in the same run.
                    const RouteDecision rd = Router::pick(s.title + "\n" + s.prompt);
                    s.model = rd.model;
                    s.route = rd.tier;
                } else if (s.backend == sessionBackend) {
                    s.model = sessionModel;
                } else if (auto b = Backends::get(s.backend)) {
                    s.model = b->defaultModel();
                }
            }
            if (!s.title.isEmpty()) subs.append(s);
        }
    }
    if (subs.isEmpty()) {
        phase("error", "the director produced no subtasks");
        return out;
    }
    out.subtasks = subs;

    // Always show who is doing what on which model — the "different models for
    // different parts" view, whether they were routed or set by hand.
    if (ev.onLog) {
        ev.onLog(QStringLiteral("model plan%1:").arg(opts.route ? " (routed)" : ""));
        for (const auto& s : subs) {
            ev.onLog(QStringLiteral("  coder #%1 [%2] %3:%4%5")
                         .arg(s.n)
                         .arg(s.role, s.backend, s.model,
                              s.route.isEmpty() ? QString() : QStringLiteral(" · %1").arg(s.route)));
        }
    }
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
    // Starter skills whose triggers match this crew's focus. They are written into
    // each sandbox, where the coder discovers them as ordinary project skills and
    // pulls each body on demand via the `skill` tool — the system prompt only ever
    // sees their names. A user skill of the same name is never overwritten.
    const QVector<SkillSpec> starters = CrewSkills::resolve(opts.focus);
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
                CrewSkills::materialize(starters, sandboxes[i]);
                return QJsonObject{{"err", err}};
            });
    }
    if (!starters.isEmpty() && ev.onLog) {
        QStringList names;
        for (const SkillSpec& s : starters) names << s.name;
        ev.onLog(QStringLiteral("skills: %1").arg(names.join(QStringLiteral(", "))));
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

            // mkpath, not just open: with --no-research nothing else has created
            // the run dir yet, and QFile::open(Append) will not create it — so the
            // coder logs silently went nowhere, which is precisely when you need them.
            QDir().mkpath(runDir(runId));
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
            // The persona the Director assigned. Unknown roles resolve to 'coder',
            // so a hallucinated role never leaves a coder without instructions.
            sys += CrewRoles::persona(st.role);
            // Name-and-description only — the bodies stay on disk until the model
            // asks for one with the `skill` tool. Rooted at the SANDBOX, which is
            // where this coder's starters were just materialised.
            const QString skills = Skills::catalogFor(sandboxes[i]);
            if (!skills.isEmpty())
                sys += "\n\nSKILLS available (load one with the skill tool before you start if it "
                       "applies):\n" + skills;
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
            const QString finalText =
                a.loop(msgs, Config::integer("crew.coderIterations", 10), sink, cancel);

            // A CLI backend streams nothing through the sink (it is a subprocess
            // running its own loop), so without this its log would be empty and a
            // failure would be invisible.
            if (!finalText.isEmpty()) {
                QFile f(log);
                if (f.open(QIODevice::Append)) f.write(finalText.toUtf8() + "\n");
            }

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
        const QString am = routedForTier(ab, opts.auditorModel, QStringLiteral("hard"));
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

    // ---- Debate (opt-in) --------------------------------------------------
    // A contested change should survive an argument, not one model's snap verdict.
    // Advocate argues to land it, Skeptic argues to hold it, Judge decides — the
    // MDASH "debate" stage, scoped to one changeset. Runs in parallel per coder,
    // and its verdict overrides the single auditor's.
    if (opts.debate && !cancel.cancelled()) {
        phase("debate", "arguing every changeset");
        const QString jb = backendFor(opts.auditorBackend);
        const QString jm = routedForTier(jb, opts.auditorModel, QStringLiteral("hard"));
        parallelRun(
            results.size(), [&](int) { return limiterKey(jb, jm); },
            [&](int i) -> QJsonObject {
                if (results[i].empty || cancel.cancelled()) return {};
                auto backend = Backends::get(jb);
                if (!backend) return {};
                const QString subtask = subs[i].title;
                const QString diff = results[i].diff.left(14000);

                auto argue = [&](const QString& stance) -> QString {
                    const QString sys =
                        "You are a code reviewer in a debate. Argue the " + stance +
                        " case ONLY, in 2-4 sentences — do not hedge. Reply with plain text.";
                    const QString usr = "Subtask: " + subtask + "\n\nProposed change:\n" + diff;
                    const QJsonObject r = backend->chatJson(
                        jm,
                        {{"system", sys + " Wrap your argument as {\"argument\":\"...\"}.", {}, {}, {}, {}, {}},
                         {"user", usr, {}, {}, {}, {}, {}}},
                        cancel);
                    return r.value("argument").toString();
                };
                const QString forCase = argue(QStringLiteral("FOR landing this change"));
                const QString againstCase = argue(QStringLiteral("AGAINST landing this change"));

                const QString jsys =
                    "You are the Judge of a code-review debate. Weigh both arguments against the "
                    "actual diff and decide whether the change is safe to land. Reply with JSON "
                    "only: {\"clean\":true|false,\"summary\":\"one line verdict\"}";
                const QString jusr = "Subtask: " + subtask + "\n\nDiff:\n" + diff +
                                     "\n\nADVOCATE:\n" + forCase + "\n\nSKEPTIC:\n" + againstCase;
                const QJsonObject v = backend->chatJson(
                    jm, {{"system", jsys, {}, {}, {}, {}, {}}, {"user", jusr, {}, {}, {}, {}, {}}},
                    cancel);
                results[i].audit.clean = v.value("clean").toBool(false);
                results[i].audit.summary =
                    QStringLiteral("[debate] ") + v.value("summary").toString();
                return {};
            });
    }

    // ---- Dedupe (opt-in) --------------------------------------------------
    // The always-on overlap guard catches two coders touching the SAME file. This
    // catches two coders doing the same WORK in DIFFERENT files (the Director
    // occasionally hands out near-duplicate subtasks). A judge groups the
    // changesets; every non-primary member of a duplicate group is marked so
    // landing holds it. No model call when there is nothing that could overlap.
    QHash<int, int> dupOf;  // coder n -> the coder it duplicates
    if (opts.dedupe && !cancel.cancelled()) {
        QVector<int> live;
        for (int i = 0; i < results.size(); ++i)
            if (!results[i].empty) live.append(i);
        if (live.size() > 1) {
            phase("dedupe", "checking for duplicated work");
            const QString jb = backendFor(opts.directorBackend);
            const QString jm = routedForTier(jb, opts.directorModel, QStringLiteral("hard"));
            if (auto backend = Backends::get(jb)) {
                QString listing;
                for (int i : live)
                    listing += QStringLiteral("coder #%1: %2 — files: %3\n")
                                   .arg(subs[i].n)
                                   .arg(subs[i].title, results[i].files.join(", "));
                const QString sys =
                    "Several coders worked in parallel. Identify groups that did the SAME work "
                    "(duplicates), even in different files. Reply with JSON only: "
                    "{\"duplicates\":[{\"keep\":<coder#>,\"drop\":[<coder#>,...]}]} — keep the "
                    "most complete member of each group. Empty list if none duplicate.";
                const QJsonObject v = backend->chatJson(
                    jm, {{"system", sys, {}, {}, {}, {}, {}}, {"user", listing, {}, {}, {}, {}, {}}},
                    cancel);
                for (const auto& g : v.value("duplicates").toArray()) {
                    const int keep = g.toObject().value("keep").toInt();
                    for (const auto& d : g.toObject().value("drop").toArray())
                        if (d.toInt() != keep) dupOf.insert(d.toInt(), keep);
                }
            }
        }
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

        // Dedupe verdict (only populated when --dedupe ran): a coder whose work
        // duplicates another's is held rather than landed twice.
        if (dupOf.contains(r.n)) {
            hold(r, QStringLiteral("duplicates coder #%1").arg(dupOf.value(r.n)));
            continue;
        }

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
    // ---- Learn (opt-in) ---------------------------------------------------
    // Distil what this run taught into durable memory, and — when the run
    // produced something with a reusable shape — a skill. Next run loads both.
    if (opts.learn && !cancel.cancelled()) {
        phase("learn", "distilling what this run taught");
        QString summary = "Task: " + opts.task + "\n\n";
        for (const auto& r : results) {
            if (r.n == 0) continue;
            summary += QStringLiteral("- coder #%1 (%2): %3 — files: %4\n")
                           .arg(r.n)
                           .arg(out.applied.contains(r.n) ? "applied" : "held",
                                r.audit.summary.isEmpty() ? subs[r.n - 1].title : r.audit.summary,
                                r.files.join(", "));
        }
        if (!research.isEmpty()) summary += "\nResearch:\n" + research.left(3000);

        const QString lb = backendFor(opts.directorBackend);
        const QString lm = routedForTier(lb, opts.directorModel, QStringLiteral("moderate"));
        const QStringList facts = Memory::autoRemember(summary, lb, lm);

        // A reusable skill, only when the run actually applied something.
        int skillsWritten = 0;
        if (!out.applied.isEmpty()) {
            if (auto backend = Backends::get(lb)) {
                const QString sys =
                    "From this completed work, extract AT MOST ONE reusable project convention "
                    "worth remembering as a skill (how this codebase does a recurring thing). Only "
                    "if there is a genuine, reusable pattern — otherwise reply {}. JSON only: "
                    "{\"name\":\"kebab-name\",\"description\":\"one line\",\"body\":\"markdown "
                    "instructions\"}";
                const QJsonObject s = backend->chatJson(
                    lm, {{"system", sys, {}, {}, {}, {}, {}}, {"user", summary, {}, {}, {}, {}, {}}},
                    cancel);
                const QString name = s.value("name").toString().trimmed();
                const QString body = s.value("body").toString().trimmed();
                if (!name.isEmpty() && !body.isEmpty()) {
                    Skills::save(name, s.value("description").toString(), body);
                    ++skillsWritten;
                }
            }
        }
        if (ev.onLog)
            ev.onLog(QStringLiteral("learned: %1 fact(s), %2 skill(s)")
                         .arg(facts.size())
                         .arg(skillsWritten));
    }

    // ---- Token efficiency -------------------------------------------------
    // The point people care about: what did this run cost, and how much did
    // routing save? "Cost" here is the free-local vs paid-cloud split, because
    // that is the part that actually shows up on a bill — local Ollama tokens are
    // free, cloud tokens are not.
    if (ev.onLog) {
        const QMap<QString, Usage::Tally> after = Usage::snapshot();
        qint64 local = 0, cloud = 0;
        QStringList lines;
        for (auto it = after.constBegin(); it != after.constEnd(); ++it) {
            const qint64 d = it.value().total() - usageBefore.value(it.key()).total();
            if (d <= 0) continue;
            (Models::isCloud(it.key()) ? cloud : local) += d;
            lines << QStringLiteral("  %1  %2 tokens%3")
                         .arg(it.key())
                         .arg(d)
                         .arg(Models::isCloud(it.key()) ? QStringLiteral(" (cloud)") : QString());
        }
        const qint64 total = local + cloud;
        if (total > 0) {
            ev.onLog(QStringLiteral("token report — %1 tokens this run:").arg(total));
            for (const QString& l : lines) ev.onLog(l);
            ev.onLog(QStringLiteral("  %1%% ran on free local models%2")
                         .arg(local * 100 / total)
                         .arg(opts.route ? QStringLiteral(" — routing kept the cheap work off the "
                                                          "paid models")
                                         : QString()));
        }
    }

    phase("done", QStringLiteral("%1 applied · %2 held")
                      .arg(out.applied.size())
                      .arg(out.held.size()));
    return out;
}

// Security-scan mode: no code is written. Scanners read the tree in parallel and
// report vulnerabilities; SecScan's regex signals seed them cheaply; the findings
// are written to a report. This is the MDASH-shaped "hunt, don't build" flow, at
// this project's scale.
Crew::Result Crew::securityScan(const CrewOptions& opts, const CrewEvents& ev,
                                const CancelToken& cancel) {
    Result out;
    const QString projectRoot = QDir::currentPath();
    const QString runId = newRunId();
    out.runId = runId;

    const QString sessionBackend = Config::str("model.backend", "ollama");
    const QString sessionModel = Config::str("ollama.defaultModel", "");
    auto phase = [&](const QString& p, const QString& m) {
        if (ev.onPhase) ev.onPhase(p, m);
    };
    const QString sb = opts.coderBackend.isEmpty() ? sessionBackend : opts.coderBackend;
    const QString sm = [&] {
        if (!opts.coderModel.isEmpty()) return opts.coderModel;
        if (opts.route && sb == QLatin1String("ollama")) return Router::modelForTier("hard");
        return sessionModel;
    }();

    // Cheap first pass: SecScan's regex signals across the tree. Fast, deterministic,
    // and a useful seed even before any model reasons about the code.
    phase("scan", "regex pre-scan");
    QString seeded;
    int seededCount = 0;
    const QHash<QString, QString> files = Sandbox::listFiles(projectRoot);
    QStringList sources = files.keys();
    std::sort(sources.begin(), sources.end());
    for (const QString& rel : sources) {
        for (const Finding& f : SecScan::scanFile(files.value(rel))) {
            seeded += QStringLiteral("- %1:%2 [%3] %4 (%5)\n")
                          .arg(rel)
                          .arg(f.line)
                          .arg(f.severity, f.rule, f.redacted);
            ++seededCount;
        }
    }
    if (ev.onLog) ev.onLog(QStringLiteral("regex pre-scan: %1 signal(s)").arg(seededCount));

    // Split the source list into scanner groups (bounded by the swarm cap), one
    // read-only agent per group hunting for vulnerabilities.
    const int cap = opts.swarmMax > 0 ? qMin(opts.swarmMax, 64) : 8;
    const int groups = qBound(1, qMin(opts.maxCoders > 0 ? opts.maxCoders : 4, cap),
                              qMax(1, sources.size()));
    QVector<QStringList> buckets(groups);
    for (int i = 0; i < sources.size(); ++i) buckets[i % groups].append(sources[i]);

    // Each scanner is a board card, so a security scan shows up on the kanban like
    // any crew — same To-do/Doing/Done columns, just scanners instead of coders.
    const QString boardTask = QStringLiteral("🛡 security scan") +
                              (opts.focus.isEmpty() ? QString() : ": " + opts.focus);
    QVector<Subtask> scanners(groups);
    for (int g = 0; g < groups; ++g) {
        scanners[g].n = g + 1;
        scanners[g].role = QStringLiteral("scanner");
        scanners[g].title = QStringLiteral("scan %1 file(s)").arg(buckets[g].size());
        scanners[g].state = "todo";
        scanners[g].backend = sb;
        scanners[g].model = sm;
    }
    publishBoard(runId, boardTask, scanners, true);

    phase("hunt", QStringLiteral("%1 scanners reading the tree").arg(groups));
    const auto reports = parallelRun(
        groups, [&](int) { return limiterKey(sb, sm); },
        [&](int g) -> QJsonObject {
            if (cancel.cancelled() || buckets[g].isEmpty()) return {};
            scanners[g].state = "doing";
            if (ev.onCoderState) ev.onCoderState(g + 1, "doing");
            publishBoard(runId, boardTask, scanners, true);
            Agent a(sb, sm);
            Permission::setMode(PermMode::ReadOnly);
            Permission::setInteractive(false);
            Tools::setThreadRoot(projectRoot);
            QString sys =
                "You are a security scanner. Using READ-ONLY tools (view, grep, glob), inspect "
                "the listed files for real vulnerabilities: injection, auth/authz gaps, unsafe "
                "deserialization, path traversal, secrets, unsafe shell/exec, SSRF, memory safety. "
                "Report ONLY concrete findings, each as `- <file>:<line> [severity] <issue> — <why>`. "
                "If you find nothing real, say so. Do NOT invent issues.";
            QString usr = "Focus" + (opts.focus.isEmpty() ? QString() : ": " + opts.focus) +
                          "\n\nFiles to scan:\n" + buckets[g].join('\n');
            if (!seeded.isEmpty())
                usr += "\n\nRegex pre-scan signals (verify these, they are not proof):\n" +
                       seeded.left(4000);
            QVector<ChatMessage> msgs{{"system", sys, {}, {}, {}, {}, {}},
                                      {"user", usr, {}, {}, {}, {}, {}}};
            const QString finding =
                a.loop(msgs, Config::integer("crew.researchIterations", 6), {}, cancel);
            // A scanner that turned up something concrete is "held" (needs your
            // eyes); a clean area is "done".
            const bool hit = finding.contains(QLatin1Char('[')) &&
                             !finding.toLower().contains(QStringLiteral("no ")) &&
                             !finding.trimmed().isEmpty();
            scanners[g].state = hit ? "held" : "done";
            if (ev.onCoderState) ev.onCoderState(g + 1, scanners[g].state);
            publishBoard(runId, boardTask, scanners, true);
            return QJsonObject{{"g", g + 1}, {"text", finding}};
        });

    // Assemble the report.
    QString report = QStringLiteral("# Security scan — %1\n\n").arg(runId);
    if (seededCount)
        report += QStringLiteral("## Regex pre-scan (%1 signal(s))\n\n%2\n").arg(seededCount).arg(seeded);
    report += QStringLiteral("## Scanner findings\n\n");
    for (const auto& r : reports) {
        const QString t = r.value("text").toString().trimmed();
        if (!t.isEmpty())
            report += QStringLiteral("### Scanner %1\n\n%2\n\n")
                          .arg(r.value("g").toInt())
                          .arg(t);
    }
    const QString path = runDir(runId) + "/security-report.md";
    writeFile(path, report);
    publishBoard(runId, boardTask, scanners, false);  // scan finished — board goes idle
    if (ev.onLog) ev.onLog(QStringLiteral("report written: %1").arg(path));
    phase("done", QStringLiteral("scanned %1 files across %2 scanners")
                      .arg(sources.size())
                      .arg(groups));
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
