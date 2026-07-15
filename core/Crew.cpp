// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

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

// The immutable plan, written ONCE when the Director finishes. current.json
// tracks live state but drops the coder prompts; plan.json keeps them (plus the
// task/focus/land) so an interrupted run can be resumed with no model calls to
// rebuild it. The two files together are everything `crew resume` needs.
struct PlanFlags {
    bool audit = true;
    bool learn = false;
    bool route = false;
    bool debate = false;
    bool dedupe = false;
    int amplify = 1;
};

void writePlan(const QString& runId, const QString& task, const QString& focus, const QString& land,
               const PlanFlags& f, const QVector<Subtask>& subs) {
    QJsonArray arr;
    for (const auto& s : subs) {
        arr.append(QJsonObject{{"n", s.n},
                               {"title", s.title},
                               {"role", s.role},
                               {"prompt", s.prompt},
                               {"backend", s.backend},
                               {"model", s.model},
                               {"route", s.route}});
    }
    QJsonObject o{{"task", task},       {"focus", focus},     {"land", land},
                  {"audit", f.audit},   {"learn", f.learn},   {"route", f.route},
                  {"debate", f.debate}, {"dedupe", f.dedupe}, {"amplify", f.amplify},
                  // The project this run belongs to, so a launcher can offer to resume
                  // only the runs that match the folder you just opened. crew is run
                  // with cwd == the project, so currentPath() is that project.
                  {"cwd", QDir::currentPath()},
                  {"subtasks", arr}};
    writeFile(runDir(runId) + "/plan.json", QString::fromUtf8(json::encode(o)));
}

QJsonObject readPlan(const QString& runId) {
    QFile f(runDir(runId) + "/plan.json");
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

}  // namespace

Crew::Result Crew::run(const CrewOptions& opts, const CrewEvents& ev, const CancelToken& cancel) {
    if (opts.security) return securityScan(opts, ev, cancel);

    Result out;
    const QString projectRoot = QDir::currentPath();
    const bool resuming = !opts.resumeRunId.isEmpty();
    // Resume brings the Director back by default to re-plan the leftover work;
    // --replay (opts.replay) opts out to a literal re-run of the saved subtasks.
    const bool replan = resuming && !opts.replay;
    const QString runId = resuming ? opts.resumeRunId : newRunId();
    out.runId = runId;

    // On resume the plan — task, focus, land mode, and every coder's prompt/model
    // — is read back from disk, because `opts` may be bare (the user just typed
    // `crew resume`). A missing plan.json means the interrupted run never got past
    // the Director; there is nothing to replay (but --replan can still rebuild it
    // as long as the saved task survives).
    const QJsonObject plan = resuming ? readPlan(runId) : QJsonObject{};
    if (resuming && plan.isEmpty() && (!replan || plan.value("task").toString().isEmpty())) {
        if (ev.onPhase) ev.onPhase("error", "nothing to resume for " + runId);
        return out;
    }
    const QString task = resuming ? plan.value("task").toString(opts.task) : opts.task;
    const QString focus = resuming ? plan.value("focus").toString(opts.focus) : opts.focus;
    const QString landMode = resuming ? plan.value("land").toString(opts.land) : opts.land;
    const bool doAudit = resuming ? plan.value("audit").toBool(opts.audit) : opts.audit;
    const bool doLearn = resuming ? plan.value("learn").toBool(opts.learn) : opts.learn;
    // The brain settings ride in the plan too, so a crew launched with routing /
    // debate / dedupe keeps them when resumed — the re-planned leftovers get the
    // same treatment the original run had.
    const bool doRoute = resuming ? plan.value("route").toBool(opts.route) : opts.route;
    const bool doDebate = resuming ? plan.value("debate").toBool(opts.debate) : opts.debate;
    const bool doDedupe = resuming ? plan.value("dedupe").toBool(opts.dedupe) : opts.dedupe;
    // Self-consistency rides in the plan too, so a resumed run re-plans its
    // leftovers with the same rigour the original run was given. Clamped: past a
    // handful of samples the consensus stops moving and you are just buying tokens.
    const int amplify =
        qBound(1, resuming ? plan.value("amplify").toInt(opts.amplify) : opts.amplify, 9);

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
        if (doRoute && backendId == QLatin1String("ollama")) return Router::modelForTier(tier);
        return modelFor(backendId, want);
    };

    // ---- Researcher (read-only, shared context for everyone downstream) ----
    QString research;
    if (resuming) {
        // The Researcher already ran; reuse its findings verbatim.
        QFile rf(runDir(runId) + "/research.md");
        if (rf.open(QIODevice::ReadOnly)) research = QString::fromUtf8(rf.readAll());
    } else if (opts.research && !cancel.cancelled()) {
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
    if (doLearn) {
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

    // ---- Plan: fresh Director, replayed from disk, or re-planned leftovers ----
    // Swarm mode raises the ceiling; the per-backend limiter still throttles real
    // concurrency, so a big number just means more queued work, not more GPU load.
    const int cap = opts.swarmMax > 0 ? qMin(opts.swarmMax, 64) : 8;
    const int maxCoders = qBound(1, opts.maxCoders, cap);
    QVector<Subtask> subs;

    // The Director, factored out so a fresh run AND a --replan can both call it.
    // It appends new subtasks to `subs`, numbering them from startN+1 so
    // re-planned work never collides with the finished coders we keep. When
    // `doneNote` is non-empty the Director is told what is already finished and
    // asked to plan ONLY the remaining work. Returns how many subtasks it added.
    auto runDirector = [&](int startN, const QString& doneNote) -> int {
        phase("plan", doneNote.isEmpty() ? "director is decomposing the task"
                                         : "director is planning the remaining work");
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
        QString usr = "Task: " + task;
        if (!focus.isEmpty()) usr += "\nFocus: " + focus;
        if (!doneNote.isEmpty())
            usr += "\n\nThese parts are ALREADY DONE — do NOT plan them again; plan ONLY the work "
                   "still needed to finish the task:\n" + doneNote;
        if (!research.isEmpty()) usr += "\n\nResearch findings:\n" + research.left(6000);

        auto backend = Backends::get(d.backendId());

        // One plan, one model call.
        //
        // Models do not reliably honour the schema on the first try — a cloud
        // model in particular will sometimes answer in prose. One firm retry
        // costs a few seconds and turns an intermittent "no subtasks" failure
        // into a non-event.
        auto planOnce = [&]() -> QJsonArray {
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
            return planned;
        };

        // Self-consistency: draw `amplify` independent plans and keep the one whose
        // subtask COUNT is the most common. Planning variance is where a weak model
        // hurts most — it will split a task 3 ways once and 6 ways the next time —
        // and the modal shape is the one it actually believes. Cheap because the
        // Director is one call; the coders are unaffected.
        QJsonArray planned;
        if (amplify <= 1) {
            planned = planOnce();
        } else {
            QVector<QJsonArray> cands;
            for (int i = 0; i < amplify && !cancel.cancelled(); ++i) {
                const QJsonArray p = planOnce();
                if (!p.isEmpty()) cands.append(p);
            }
            if (!cands.isEmpty()) {
                QHash<int, int> freq;  // subtask count -> how many plans agreed on it
                for (const auto& c : cands) ++freq[c.size()];
                int modeCount = cands.first().size(), best = 0;
                for (auto it = freq.constBegin(); it != freq.constEnd(); ++it) {
                    // Ties break toward the SMALLER plan: fewer coders is the cheaper
                    // and less conflict-prone bet when the model is genuinely torn.
                    if (it.value() > best || (it.value() == best && it.key() < modeCount)) {
                        best = it.value();
                        modeCount = it.key();
                    }
                }
                for (const auto& c : cands) {
                    if (c.size() == modeCount) {
                        planned = c;
                        break;
                    }
                }
                if (ev.onLog)
                    ev.onLog(QStringLiteral("director drew %1 plans · %2 agreed on %3 subtasks")
                                 .arg(cands.size())
                                 .arg(best)
                                 .arg(modeCount));
            }
        }

        int added = 0, n = startN;
        for (const auto& v : planned) {
            if (added >= maxCoders) break;
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
                } else if (doRoute && s.backend == QLatin1String("ollama")) {
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
            if (!s.title.isEmpty()) {
                subs.append(s);
                ++added;
            }
        }
        return added;
    };

    if (!resuming) {
        runDirector(0, QString());
    } else {
        // Replay the interrupted run's live state to see which coders finished.
        // A "done" coder BUILT a changeset (it is re-landed from disk, never
        // re-run); a "held" coder is already awaiting a human on the board. Both
        // are kept as-is and are NEVER redone. Only "todo"/"doing" coders were
        // genuinely cut off.
        QHash<int, QString> liveState;
        for (const auto& v : boardState().value("subtasks").toArray()) {
            const auto o = v.toObject();
            liveState.insert(o.value("n").toInt(), o.value("state").toString());
        }
        QStringList doneTitles;
        int maxN = 0, kept = 0, rerun = 0;
        for (const auto& v : plan.value("subtasks").toArray()) {
            const auto o = v.toObject();
            Subtask s;
            s.n = o.value("n").toInt();
            s.title = o.value("title").toString();
            s.role = o.value("role").toString("coder");
            s.prompt = o.value("prompt").toString(s.title);
            s.backend = o.value("backend").toString();
            s.model = o.value("model").toString();
            s.route = o.value("route").toString();
            maxN = qMax(maxN, s.n);
            const QString st = liveState.value(s.n);
            if (st == "done" || st == "held") {
                s.state = st;  // finished — kept, landed from disk, never re-run
                if (st == "done") doneTitles << s.title;
                if (!s.title.isEmpty()) {
                    subs.append(s);
                    ++kept;
                }
            } else if (!replan) {
                s.state = "todo";  // plain resume: re-run this interrupted coder
                if (!s.title.isEmpty()) {
                    subs.append(s);
                    ++rerun;
                }
            }
            // --replan: an unfinished coder's OLD subtask is dropped here; the
            // Director re-plans the remaining work just below.
        }
        if (replan) {
            const QString note = doneTitles.isEmpty() ? QString() : "- " + doneTitles.join("\n- ");
            rerun = runDirector(maxN, note);
            phase("resume", QStringLiteral("kept %1 finished · re-planned %2 to run").arg(kept).arg(rerun));
        } else {
            phase("resume", QStringLiteral("%1 finished · %2 to run").arg(kept).arg(rerun));
        }
    }
    if (subs.isEmpty()) {
        phase("error", resuming ? "the saved plan had no subtasks" : "the director produced no subtasks");
        return out;
    }
    out.subtasks = subs;

    // Persist the plan the moment it exists (fresh runs only — on resume it is
    // already on disk). From here on an interruption is recoverable: plan.json has
    // the prompts, current.json has the live state.
    if (!resuming || replan)
        writePlan(runId, task, focus, landMode,
                  PlanFlags{doAudit, doLearn, doRoute, doDebate, doDedupe, amplify}, subs);

    // Always show who is doing what on which model — the "different models for
    // different parts" view, whether they were routed or set by hand.
    if (ev.onLog) {
        ev.onLog(QStringLiteral("model plan%1:").arg(doRoute ? " (routed)" : ""));
        for (const auto& s : subs) {
            ev.onLog(QStringLiteral("  coder #%1 [%2] %3:%4%5")
                         .arg(s.n)
                         .arg(s.role, s.backend, s.model,
                              s.route.isEmpty() ? QString() : QStringLiteral(" · %1").arg(s.route)));
        }
    }
    publishBoard(runId, task, subs, true);

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
    QVector<CoderResult> results(subs.size());
    for (int i = 0; i < subs.size(); ++i) {
        sandboxes[i] = wtRoot + QStringLiteral("/c%1").arg(i + 1);
        stores[i] = csRoot + QStringLiteral("/c%1").arg(i + 1);
    }
    // On resume a coder is skipped when it is "held" (already awaiting a human on
    // the board) or "done". A "done" coder BUILT its changeset before the run was
    // cut off: it is reloaded from disk and landed as-is — never re-run and never
    // re-audited (we trust work that already passed), so a finished coder costs
    // zero model calls on resume. Only "todo" coders get a fresh sandbox and run.
    auto skipRun = [&](int i) { return subs[i].state == "done" || subs[i].state == "held"; };
    QSet<int> reloaded;  // finished-and-reloaded coders — landed from disk, not re-audited
    for (int i = 0; i < subs.size(); ++i)
        if (subs[i].state == "done") reloaded.insert(i);
    // Starter skills whose triggers match this crew's focus. They are written into
    // each sandbox, where the coder discovers them as ordinary project skills and
    // pulls each body on demand via the `skill` tool — the system prompt only ever
    // sees their names. A user skill of the same name is never overwritten.
    const QVector<SkillSpec> starters = CrewSkills::resolve(focus);
    {
        // Copying is pure I/O, so it fans out freely — this key has no model behind it.
        Limiter::instance().setLimit("io", QThread::idealThreadCount());
        parallelRun(
            subs.size(), [](int) { return QStringLiteral("io"); },
            [&](int i) -> QJsonObject {
                if (subs[i].state == "done") {  // resume: reuse the captured changeset
                    const Changeset cs = Sandbox::load(stores[i]);
                    CoderResult r;
                    r.n = subs[i].n;
                    r.empty = cs.empty();
                    r.store = cs.store;
                    r.files = cs.files();
                    r.diff = cs.diff;
                    r.audit.clean = true;  // already passed once — land it, don't re-audit
                    results[i] = r;
                    return {};
                }
                if (subs[i].state == "held") return {};  // resume: leave it on the board
                QString err;
                // A git worktree when the project is under git (shared object store,
                // and the coder gets a REAL repo it can run git_diff/git_log in), a
                // folder copy when it is not. See Sandbox::create.
                Sandbox::create(projectRoot, sandboxes[i], &err);
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

    parallelRun(
        subs.size(),
        [&](int i) { return limiterKey(subs[i].backend, subs[i].model); },
        [&](int i) -> QJsonObject {
            const Subtask& st = subs[i];
            if (cancel.cancelled() || skipRun(i)) return {};

            subs[i].state = "doing";
            if (ev.onCoderState) ev.onCoderState(st.n, "doing");
            publishBoard(runId, task, subs, true);

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
            QString usr = "Overall goal: " + task + "\n\nYour subtask: " + st.title + "\n" +
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
            // Tool activity, logged in a grammar the desktop pane parses back out:
            //   → edit src/Parser.cpp
            // Without this the log is a wall of raw model prose, and a watcher
            // cannot tell a coder that is working from one that is waffling.
            sink.onTool = [&, n = st.n](const QString& tool, const QString& detail) {
                QFile f(log);
                if (f.open(QIODevice::Append))
                    f.write(QStringLiteral("\n→ %1%2\n")
                                .arg(tool, detail.isEmpty() ? QString()
                                                            : QStringLiteral(" ") + detail)
                                .toUtf8());
            };

            // Live steering. `steer.jsonl` was WRITE-ONLY until now: Crew::steer
            // appended to it, the board's steer box wrote into it, and absolutely
            // nothing ever read it back — so talking to a running coder did nothing
            // at all. Each coder now drains the lines addressed to it (target 0 =
            // the whole crew) at the top of each iteration, and remembers how far it
            // has read so a message is delivered exactly once.
            const QString steerFile = runDir(runId) + QStringLiteral("/steer.jsonl");
            qint64 steerSeen = 0;
            auto drainSteer = [&, n = st.n]() -> QString {
                QFile f(steerFile);
                if (!f.open(QIODevice::ReadOnly)) return {};
                if (!f.seek(steerSeen)) return {};
                QStringList words;
                while (!f.atEnd()) {
                    const QByteArray raw = f.readLine();
                    const QJsonObject o = QJsonDocument::fromJson(raw).object();
                    const int target = o.value("target").toInt();
                    const QString msg = o.value("msg").toString();
                    if (msg.isEmpty()) continue;
                    if (target == 0 || target == n) words << msg;
                }
                steerSeen = f.pos();
                if (words.isEmpty()) return {};
                if (ev.onLog)
                    ev.onLog(QStringLiteral("coder #%1 heard you: %2").arg(n).arg(words.join("; ")));
                QFile lf(log);
                if (lf.open(QIODevice::Append))
                    lf.write(QStringLiteral("\n[you said: %1]\n").arg(words.join("; ")).toUtf8());
                return QStringLiteral(
                           "The human watching you says: %1\n\nTake this into account from here on.")
                    .arg(words.join(QStringLiteral("\n")));
            };

            // Thread-local, NOT QDir::setCurrent — cwd is process-wide and
            // parallel coders would stomp each other into the wrong sandbox.
            Tools::setThreadRoot(sandboxes[i]);
            const QString finalText = a.loop(msgs, Config::integer("crew.coderIterations", 10),
                                             sink, cancel, drainSteer);

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
            publishBoard(runId, task, subs, true);
            return {};
        });

    // ---- Auditors, also in parallel ---------------------------------------
    if (doAudit && !cancel.cancelled()) {
        phase("audit", amplify > 1
                           ? QStringLiteral("%1-reviewer panel on every changeset").arg(amplify)
                           : QStringLiteral("reviewing every changeset"));
        const QString ab = backendFor(opts.auditorBackend);
        const QString am = routedForTier(ab, opts.auditorModel, QStringLiteral("hard"));
        parallelRun(
            results.size(), [&](int) { return limiterKey(ab, am); },
            [&](int i) -> QJsonObject {
                // Reloaded (already-finished) coders keep their prior clean verdict —
                // resume never re-audits work that already passed.
                if (results[i].empty || reloaded.contains(i) || cancel.cancelled()) return {};
                auto backend = Backends::get(ab);
                if (!backend) return {};

                // One reviewer's verdict. A skeptic is told to hunt for a reason to
                // reject — with amplify on, every other seat is one, so a change has
                // to convince an adversary and not just a neutral reader.
                auto reviewOnce = [&](bool skeptic) -> AuditResult {
                    const QString stance =
                        skeptic
                            ? QStringLiteral(
                                  "You are a SKEPTICAL adversarial reviewer. Actively hunt for a "
                                  "reason to reject this changeset; when in doubt, mark it unclean. ")
                            : QStringLiteral("You are the Auditor. ");
                    const QString sys =
                        stance +
                        "Review this changeset for correctness, security, and scope creep. Mark it "
                        "unclean if it is wrong, unsafe, or does work outside the stated subtask. "
                        "Reply with JSON only: "
                        "{\"clean\":true|false,\"summary\":\"one line\",\"issues\":[\"...\"]}";
                    const QString usr =
                        "Subtask: " + subs[i].title + "\n\nDiff:\n" + results[i].diff.left(16000);
                    const QJsonObject v = backend->chatJson(
                        am, {{"system", sys, {}, {}, {}, {}, {}}, {"user", usr, {}, {}, {}, {}, {}}},
                        cancel);
                    AuditResult a;
                    a.clean = v.value("clean").toBool(false);
                    a.summary = v.value("summary").toString();
                    for (const auto& is : v.value("issues").toArray()) a.issues << is.toString();
                    return a;
                };

                if (amplify <= 1) {
                    results[i].audit = reviewOnce(false);
                    return {};
                }

                // The panel: alternate neutral and skeptic seats, and land the change
                // only on a STRICT majority of clean votes. A 2-2 split holds it.
                int clean = 0;
                AuditResult panel;
                for (int pass = 0; pass < amplify && !cancel.cancelled(); ++pass) {
                    const AuditResult a = reviewOnce(pass % 2 == 1);
                    if (a.clean) {
                        ++clean;
                    } else {
                        for (const QString& is : a.issues)
                            if (!panel.issues.contains(is)) panel.issues << is;
                    }
                    if (panel.summary.isEmpty()) panel.summary = a.summary;
                }
                panel.clean = clean * 2 > amplify;
                panel.summary = QStringLiteral("%1/%2 reviewers clean%3")
                                    .arg(clean)
                                    .arg(amplify)
                                    .arg(panel.summary.isEmpty()
                                             ? QString()
                                             : QStringLiteral(" · ") + panel.summary);
                results[i].audit = panel;
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
    if (doDebate && !cancel.cancelled()) {
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
    if (doDedupe && !cancel.cancelled()) {
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
    const bool reviewMode = (landMode == "review");
    QHash<QString, int> touched;  // file -> the coder that already claimed it

    // results[i] is aligned with subs[i]; index by i, NOT by coder number (a
    // --replan renumbers new coders past the kept ones, so n-1 is not the index).
    auto hold = [&](int i, const QString& reason) {
        const CoderResult& r = results[i];
        out.held.append(r.n);
        subs[i].state = "held";
        Decision d;
        d.kind = "crew_branch";
        d.summary = QStringLiteral("coder #%1 — %2").arg(r.n).arg(subs[i].title);
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

    for (int i = 0; i < results.size(); ++i) {
        CoderResult& r = results[i];
        if (r.n == 0 || r.empty) continue;

        // Dedupe verdict (only populated when --dedupe ran): a coder whose work
        // duplicates another's is held rather than landed twice.
        if (dupOf.contains(r.n)) {
            hold(i, QStringLiteral("duplicates coder #%1").arg(dupOf.value(r.n)));
            continue;
        }

        // A changeset that introduces a credential is NEVER auto-applied, no
        // matter how clean the audit was.
        QVector<Finding> high;
        for (const auto& f : SecScan::scanDiff(r.diff))
            if (f.severity == "high") high.append(f);
        if (!high.isEmpty()) {
            hold(i, QStringLiteral("secret detected (%1) — not auto-applied").arg(high.size()));
            continue;
        }
        if (reviewMode) {
            hold(i, "review mode");
            continue;
        }
        if (!r.audit.clean) {
            hold(i, "audit flagged: " + r.audit.summary);
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
            hold(i, QStringLiteral("overlaps coder #%1 on %2").arg(touched[clash]).arg(clash));
            continue;
        }

        QStringList wrote;
        QString err;
        if (!Sandbox::apply(r.store, projectRoot, &wrote, &err)) {
            hold(i, "could not write files: " + err);
            continue;
        }
        for (const auto& f : r.files) touched.insert(f, r.n);
        out.applied.append(r.n);
        subs[i].state = "done";
        if (ev.onLog)
            ev.onLog(QStringLiteral("applied #%1 (%2 files)").arg(r.n).arg(wrote.size()));
    }

    out.results = results;
    publishBoard(runId, task, subs, false);
    // destroy(), not removeTree(): a worktree has to be unregistered from the
    // project's .git as well as deleted, or its admin entry lingers and the path
    // cannot be reused by the next run.
    for (const auto& sb : sandboxes) Sandbox::destroy(projectRoot, sb);
    // ---- Learn (opt-in) ---------------------------------------------------
    // Distil what this run taught into durable memory, and — when the run
    // produced something with a reusable shape — a skill. Next run loads both.
    if (doLearn && !cancel.cancelled()) {
        phase("learn", "distilling what this run taught");
        QString summary = "Task: " + task + "\n\n";
        for (int i = 0; i < results.size(); ++i) {
            const CoderResult& r = results[i];
            if (r.n == 0) continue;
            summary += QStringLiteral("- coder #%1 (%2): %3 — files: %4\n")
                           .arg(r.n)
                           .arg(out.applied.contains(r.n) ? "applied" : "held",
                                r.audit.summary.isEmpty() ? subs[i].title : r.audit.summary,
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
            // Only claim the win when there IS one: with every tier routed to a
            // cloud model, local share is 0 and boasting that routing "kept the
            // cheap work off the paid models" is precisely backwards.
            const qint64 pct = local * 100 / total;
            ev.onLog(QStringLiteral("  %1% ran on free local models%2")
                         .arg(pct)
                         .arg(doRoute && pct > 0
                                  ? QStringLiteral(" — routing kept the cheap work off the "
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
    const QString boardTask = QStringLiteral("security scan") +
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

QString Crew::coderLog(const QString& runId, int coder, qint64 offset, qint64* size) {
    if (size) *size = 0;
    if (runId.isEmpty() || coder <= 0) return {};
    QFile f(runDir(runId) + QStringLiteral("/coder-%1.log").arg(coder));
    if (!f.open(QIODevice::ReadOnly)) return {};  // the coder has not started yet
    const qint64 len = f.size();
    if (size) *size = len;
    // A log that SHRANK means it was rotated or a new run reused the id; the old
    // offset points at nothing real, so start again rather than slice past the end.
    if (offset < 0 || offset > len) offset = 0;
    if (!f.seek(offset)) return {};
    return QString::fromUtf8(f.readAll());
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

QVector<Crew::RunInfo> Crew::resumable() {
    // The one live run current.json points at carries real per-coder state; for
    // any other run on disk we only know the plan, so "done" reads 0 there.
    const QJsonObject live = boardState();
    const QString liveId = live.value("runId").toString();
    QHash<int, QString> liveState;
    for (const auto& v : live.value("subtasks").toArray()) {
        const auto o = v.toObject();
        liveState.insert(o.value("n").toInt(), o.value("state").toString());
    }

    QVector<RunInfo> runs;
    QDir dir(Config::crewDir());
    // Newest first: run ids are crew_<unixSecs>, so reverse name order is time order.
    const QStringList names = dir.entryList({QStringLiteral("crew_*")}, QDir::Dirs, QDir::Name);
    for (int i = names.size() - 1; i >= 0; --i) {
        const QString id = names.at(i);
        const QJsonObject plan = readPlan(id);
        if (plan.isEmpty()) continue;  // never got past the Director — not resumable
        RunInfo info;
        info.runId = id;
        info.task = plan.value("task").toString();
        info.cwd = plan.value("cwd").toString();
        const QJsonArray subs = plan.value("subtasks").toArray();
        info.total = subs.size();
        if (id == liveId) {
            for (const auto& v : subs) {
                const QString st = liveState.value(v.toObject().value("n").toInt());
                if (st == "done" || st == "held") ++info.done;
            }
        }
        runs.append(info);
    }
    return runs;
}

}  // namespace odv
