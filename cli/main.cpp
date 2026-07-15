// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QJsonObject>
#include <QProcess>
#include <QTextStream>

#include <optional>

#include "Agent.h"
#include "AgentDefs.h"
#include "Backend.h"
#include "Board.h"
#include "CodeIndex.h"
#include "Config.h"
#include "ContextTuner.h"
#include "Crew.h"
#include "Eval.h"
#include "GitFlow.h"
#include "Hooks.h"
#include "Json.h"
#include "Lsp.h"
#include "Mcp.h"
#include "Memory.h"
#include "Models.h"
#include "Onboard.h"
#include "Parallel.h"
#include "Router.h"
#include "Puller.h"
#include "Repl.h"
#include "SecScan.h"
#include "Skills.h"
#include "Stt.h"
#include "Terminals.h"
#include "Tools.h"
#include "Usage.h"
#include "Verify.h"
#include "Vision.h"
#include "Acp.h"
#include "Plugins.h"
#include "Rebase.h"
#include "Update.h"
#include "Workspaces.h"
#include "Version.h"
#include "Watch.h"
#include "WebSearch.h"

using namespace odv;

namespace {

QTextStream& out() {
    static QTextStream s(stdout);
    return s;
}
QTextStream& err() {
    static QTextStream s(stderr);
    return s;
}

bool hasFlag(const QStringList& a, const QString& f) { return a.contains(f); }

QString flagValue(const QStringList& a, const QString& f, const QString& fallback = {}) {
    const int i = a.indexOf(f);
    if (i >= 0 && i + 1 < a.size()) return a.at(i + 1);
    return fallback;
}

// KeepEmptyParts, deliberately: these lists are POSITIONAL — entry i is coder i.
// `--coder-models qwen3.5:9b,,` means "coder 1 on qwen, coders 2 and 3 on their
// own backend's default". Skipping the empty slots would collapse that to a
// one-element list, and the round-robin would then wrap and hand coder 2 an
// Ollama tag that its Claude backend cannot resolve.
QStringList flagList(const QStringList& a, const QString& f) {
    const QString v = flagValue(a, f);
    if (v.isEmpty()) return {};
    return v.split(',', Qt::KeepEmptyParts);
}

// Everything that is not a flag or a flag's value: the command, or the words of a
// one-shot prompt. Used to tell `ollamadev -m qwen3.5:9b` (start the REPL on that
// model) apart from `ollamadev fix the parser` (one shot).
QStringList positionals(const QStringList& a) {
    static const QStringList takesValue{
        "--backend",         "-m",
        "--model",           "--max",
        "--parallel",        "--focus",
        "--coder-backend",   "--coder-model",
        "--coder-backends",  "--coder-models",
        "--director-backend", "--director-model",
        "--auditor-backend", "--auditor-model",
        "--researcher-backend", "--researcher-model",
        "--session",         "--swarm",
        "--amplify",         "--pack",
        "--land"};
    QStringList out;
    for (int i = 0; i < a.size(); ++i) {
        const QString& t = a.at(i);
        if (t.startsWith('-')) {
            if (takesValue.contains(t)) ++i;  // skip its value too
            continue;
        }
        out << t;
    }
    return out;
}

// Reconcile the CLI's working directory with the shared "current project" — the
// active workspace the desktop also follows (~/.ollamadev/workspaces.json). Scoped
// to folders already bookmarked as workspaces, so a throwaway run in /tmp never
// disturbs it. Three cases:
//   * cwd is at or inside a bookmarked workspace -> that IS the current project;
//     publish it as active (the desktop follows) and stay put. The deepest such
//     workspace wins, so a project nested under a broader one is picked correctly.
//   * cwd is unrelated to any workspace -> FOLLOW the active workspace: chdir into
//     it, so `ollamadev` from anywhere reopens the project you (or the desktop)
//     last had active, and auto-resume picks up its session.
//   * no active workspace -> stay in cwd (the historical behaviour).
void syncCurrentProject() {
    const QString cwdCanon = QFileInfo(QDir::currentPath()).canonicalFilePath();
    const QVector<Workspace> all = Workspaces::all();

    const Workspace* enclosing = nullptr;
    int deepest = -1;
    for (const Workspace& w : all) {
        const QString wp = QFileInfo(w.path).canonicalFilePath();
        if (wp.isEmpty()) continue;  // a bookmarked folder that no longer exists
        if (cwdCanon == wp || cwdCanon.startsWith(wp + QLatin1Char('/'))) {
            if (wp.length() > deepest) { enclosing = &w; deepest = wp.length(); }
        }
    }
    if (enclosing) {
        Workspaces::open(enclosing->id);  // mark active + bump lastOpened
        return;
    }

    const QString activeId = Workspaces::activeId();
    if (activeId.isEmpty()) return;
    for (const Workspace& w : all)
        if (w.id == activeId) {
            if (QFileInfo(w.path).isDir()) QDir::setCurrent(w.path);
            return;
        }
}

void printHelp() {
    out() << "OllamaDev " << ODV_VERSION << " — Ollama and every major coding CLI, in parallel\n\n"
          << "Usage: ollamadev [command] [options]\n\n"
          << "  ollamadev                    interactive chat (auto-resumes this folder; --new for fresh)\n"
          << "  ollamadev \"<prompt>\"        one-shot agent turn\n"
          << "  ollamadev backends           which providers are installed and how wide they run\n"
          << "  ollamadev models             list models on the active backend\n"
          << "  ollamadev models presets|cloud|chain [--json]   curated catalog + fallback chain\n"
          << "  ollamadev agents [show <n>]  file-defined subagent personas (.ollamadev/agents/*.md)\n"
          << "  ollamadev doctor             health check\n"
          << "  ollamadev setup              detect hardware, recommend + pull a model\n"
          << "  ollamadev pull <model>       download a model (resumable)\n"
          << "  ollamadev context            suggest num_ctx from free RAM/VRAM\n"
          << "  ollamadev stats              token usage this project\n\n"
          << "Chat & sessions:\n"
          << "  ollamadev chat [\"<prompt>\"]  tool-free conversation (chat mode, no file edits)\n"
          << "  ollamadev chat list|delete <id>   manage saved threads\n"
          << "  ollamadev load <id>          resume a specific session\n"
          << "  ollamadev resume             pick a recent session to resume\n\n"
          << "Config & shell:\n"
          << "  ollamadev config get <key>            read a dotted key\n"
          << "  ollamadev config set <key> <value>    write it to ade-prefs.json (not config.json)\n"
          << "  ollamadev completion bash|zsh|fish    print a sourceable completion script\n\n"
          << "Crew — the parallel bench (research → plan → N coders → audit → land):\n"
          << "  ollamadev crew \"<task>\"\n"
          << "  ollamadev crew accept <n>    apply held work into your folder\n"
          << "  ollamadev crew discard <n>   throw held work away\n"
          << "  ollamadev crew steer <n> \"…\" talk to a running coder\n"
          << "  ollamadev crew resume [id]   finish an interrupted run: keep what's done,\n"
          << "                               the Director re-plans what's left (--replay to skip it)\n"
          << "  ollamadev crew role|pack     personas the Director assigns · saved crew configs\n"
          << "  ollamadev board              pending decisions\n"
          << "  crew brain options (all opt-in — plain crew is unchanged):\n"
          << "    --route                    auto-pick each role's model by difficulty\n"
          << "    --debate                   advocate/skeptic/judge vote per changeset\n"
          << "    --dedupe                   hold coders whose work duplicates another's\n"
          << "    --security                 read-only vulnerability scan → a report\n"
          << "    --swarm N                  raise the coder cap for a bigger fan-out\n"
          << "    --amplify N                N Director plans (keep the modal one) + an\n"
          << "                               N-reviewer audit panel — majority rules\n"
          << "    --learn                    remember what this run teaches, for the next one\n"
          << "    --pack <name>              start from a saved team; your flags still win\n"
          << "  ollamadev route [--run] \"…\"  show (or run) which model the brain picks\n\n"
          << "Context:\n"
          << "  ollamadev index build        semantic code index (also: status, clear)\n"
          << "  ollamadev code-search \"<q>\"  search the repo by meaning\n"
          << "  ollamadev search \"<q>\"       web search\n"
          << "  ollamadev skills             progressive-disclosure skills (list/add/install)\n"
          << "  ollamadev memory             wiki-linked notes (new/list/show/graph)\n\n"
          << "Ship it — the AI git workflow:\n"
          << "  ollamadev diff [--json]      the working-tree diff, for review\n"
          << "  ollamadev commit [-a] [-m]   AI commit message; a leaked secret BLOCKS it\n"
          << "  ollamadev ship [--yes]       stage → scan → AI commit → ask, then push\n"
          << "  ollamadev pr create|review   draft a PR · review one with the model (needs gh)\n"
          << "  ollamadev git <sub>          status, diff, log, branch, checkout, add,\n"
          << "                               commit, push, pull, stash, show\n"
          << "  --force bypasses the secret gate. --yes only auto-answers the prompts —\n"
          << "  automation may skip the questions, it may not overrule a leaked credential.\n"
          << "  Commits run on `git.model` if set, so they can use a smaller model than chat.\n\n"
          << "Tests:\n"
          << "  ollamadev test               detect and run this project's tests\n"
          << "  ollamadev verify [--max N]   run them, and let the agent fix failures until green\n\n"
          << "Always-on:\n"
          << "  ollamadev watch \"<task>\" [paths…]   re-run a task whenever files change\n"
          << "                               --interval N (debounce) · --once (run now, then exit)\n"
          << "  ollamadev terminal create <name>    a named, long-lived pty that outlives this\n"
          << "                               command — the desktop and the CLI share them\n"
          << "  ollamadev terminal spawn <name> <cmd…>   the same, running one command\n"
          << "  ollamadev terminal list|start|stop|delete|log <name>\n"
          << "  ollamadev terminal attach <name>   your tty in, its output out (Ctrl-] detaches)\n"
          << "  ollamadev terminal send <name> \"<text>\" · terminal broadcast \"<text>\"\n\n"
          << "Automation:\n"
          << "  ollamadev hooks              shell hooks on tool/session events (list/add/remove)\n"
          << "                               PreToolUse BLOCKS the tool on a non-zero exit;\n"
          << "                               read from your HOME config only, never from a repo\n"
          << "  ollamadev commands           your own /slash commands (prompt templates)\n"
          << "  ollamadev /<name> [args]     run one as a single turn\n\n"
          << "Integration:\n"
          << "  ollamadev mcp serve          expose these tools to any MCP client (stdio)\n"
          << "  ollamadev mcp list|add|rm    MCP servers this agent can call\n"
          << "  ollamadev scan [path]        secret scanner (exit 1 on a high finding)\n"
          << "  ollamadev voice              record the mic and transcribe it (100% local)\n"
          << "                               --setup fetch the engine · --model <size> ·\n"
          << "                               --history [n] · --clear\n"
          << "  ollamadev transcribe <file>  transcribe an audio file\n"
          << "  ollamadev lsp [--port N]     language server — AI completion, hover, go-to-def\n"
          << "                               and real diagnostics (php -l, py_compile, go vet,\n"
          << "                               gcc, rustc). stdio by default; --port for TCP\n"
          << "  ollamadev eval               fixed task suite → a pass rate you can compare\n"
          << "                               --only <task> · --compare a,b,c · --json ·\n"
          << "                               --min N (exit 1 below N%) · --keep\n\n"
          << "Options:\n"
          << "  --backend <id>               ollama | claude | codex | gemini | cursor-agent |\n"
          << "                               opencode | qwen | aider | goose | amp | crush | droid\n"
          << "  -m, --model <name>\n"
          << "  --max N                      coders (default 4)\n"
          << "  --parallel N                 cap concurrency (default: each backend's real limit)\n"
          << "  --coder-backends a,b,c       one per coder — mix providers in a single crew\n"
          << "  --coder-models a,b,c\n"
          << "  --director-backend/-model, --auditor-backend/-model, --researcher-backend/-model\n"
          << "  --review                     hold everything for review instead of auto-applying\n"
          << "  --no-research, --no-audit\n"
          << "  --no-web                     block every network tool for this run\n"
          << "  --focus \"<text>\"\n\n"
          << "A model tag belongs to one backend, so the --coder-* lists are positional:\n"
          << "  --coder-backends ollama,claude,codex --coder-models qwen3.5:9b,,\n"
          << "  (coder 1 on qwen; coders 2 and 3 on their own backend's default)\n";
    out().flush();
}

int cmdBackends() {
    out() << "Backend        Installed  Native tools  Concurrency\n";
    out() << "─────────────────────────────────────────────────────\n";
    for (const auto& id : Backends::all()) {
        auto b = Backends::get(id);
        if (!b) continue;
        const bool up = b->available();
        QString conc;
        if (id == "ollama") {
            conc = QStringLiteral("%1 local / %2 cloud")
                       .arg(b->concurrencyLimit("qwen3.5:9b"))
                       .arg(b->concurrencyLimit("gpt-oss:20b-cloud"));
        } else {
            conc = QString::number(b->concurrencyLimit({}));
        }
        out() << QStringLiteral("%1 %2 %3 %4\n")
                     .arg(Backends::labelFor(id), -14)
                     .arg(up ? "yes" : "—", -10)
                     .arg(b->supportsNativeTools() ? "yes" : "own loop", -13)
                     .arg(up ? conc : QStringLiteral("—"));
    }
    out() << "\n'own loop' means the CLI does its own agentic work and its own file edits;\n"
             "we hand it a subtask and let it run.\n";
    out().flush();
    return 0;
}

// Shared by `crew` and `crew resume`: wire the progress events, run, and print
// the applied/held summary. The caller has already filled `o`.
static int runCrewAndReport(CrewOptions& o) {
    CrewEvents ev;
    ev.onPhase = [](const QString& p, const QString& m) {
        out() << "\n▸ " << p << ": " << m << "\n";
        out().flush();
    };
    ev.onLog = [](const QString& m) {
        out() << "  " << m << "\n";
        out().flush();
    };
    ev.onCoderState = [](int n, const QString& s) {
        out() << "  coder #" << n << " → " << s << "\n";
        out().flush();
    };

    CancelToken cancel;
    const auto r = Crew::run(o, ev, cancel);

    out() << "\n" << r.applied.size() << " applied · " << r.held.size() << " held\n";
    if (!r.held.isEmpty()) {
        out() << "Review held work:  ollamadev board\n"
              << "Apply it:          ollamadev crew accept <n>\n";
    }
    out().flush();
    return 0;
}

// `ollamadev crew resume [runId|list]` — finish an interrupted run. With no id it
// picks the most recent resumable run; coders that already finished are not re-run.
int cmdCrewResume(const QStringList& args) {
    const QVector<Crew::RunInfo> runs = Crew::resumable();
    if (args.value(0) == "list") {
        if (runs.isEmpty()) {
            out() << "no resumable runs\n";
            out().flush();
            return 0;
        }
        for (const auto& r : runs)
            out() << "  " << r.runId << "   " << r.done << "/" << r.total << " done   "
                  << r.task.left(60) << "\n";
        out().flush();
        return 0;
    }
    const bool replay = hasFlag(args, "--replay");  // opt out of the default re-plan
    QString runId = positionals(args).value(0);      // first non-flag word
    if (runId.isEmpty()) {
        if (runs.isEmpty()) {
            err() << "no run to resume — start one with: ollamadev crew \"…\"\n";
            err().flush();
            return 1;
        }
        runId = runs.first().runId;  // newest
    }
    out() << "resuming " << runId << (replay ? " (replaying the saved plan)\n" : " (re-planning what's left)\n");
    out().flush();
    CrewOptions o;
    o.resumeRunId = runId;
    o.replay = replay;
    o.maxCoders = flagValue(args, "--max", "4").toInt();  // bounds the Director's re-plan
    return runCrewAndReport(o);
}

int cmdCrew(const QStringList& args) {
    CrewOptions o;
    // The first non-flag word, so the task survives being written AFTER a flag
    // (`crew --pack backend "add a /health route"`), which is the natural way to
    // type it once packs exist.
    o.task = positionals(args).value(0);

    // --pack <name>: a saved team is the BASE, and an explicit flag on this command
    // line always beats it. That ordering is the whole point of a pack — it is a
    // set of defaults you got tired of retyping, not a straitjacket.
    QJsonObject pack;
    const QString packName = flagValue(args, "--pack");
    if (!packName.isEmpty()) {
        pack = CrewPacks::load(packName);
        if (pack.isEmpty()) {
            err() << "no crew pack '" << packName << "' (list them with: ollamadev crew pack list)\n";
            err().flush();
            return 1;
        }
        out() << "crew pack: " << packName << "\n";
        out().flush();
    }
    // Flag first, then the pack, then the built-in default.
    auto str = [&](const char* flag, const char* key, const QString& def = QString()) {
        const QString v = flagValue(args, flag);
        return v.isEmpty() ? pack.value(QLatin1String(key)).toString(def) : v;
    };
    auto num = [&](const char* flag, const char* key, int def) {
        const QString v = flagValue(args, flag);
        return v.isEmpty() ? pack.value(QLatin1String(key)).toInt(def) : v.toInt();
    };
    // A --no-X flag can only turn something OFF, so the pack decides only when the
    // flag is absent.
    auto onOff = [&](const char* offFlag, const char* key) {
        return hasFlag(args, offFlag) ? false : pack.value(QLatin1String(key)).toBool(true);
    };
    auto flagOr = [&](const char* flag, const char* key) {
        return hasFlag(args, flag) ? true : pack.value(QLatin1String(key)).toBool(false);
    };

    o.maxCoders = num("--max", "max", 4);
    o.parallel = flagValue(args, "--parallel", "0").toInt();
    o.focus = str("--focus", "focus");
    o.research = onOff("--no-research", "research");
    o.audit = onOff("--no-audit", "audit");
    o.route = flagOr("--route", "route");    // auto-pick each role's model by difficulty
    o.debate = flagOr("--debate", "debate");  // advocate/skeptic/judge per changeset
    o.dedupe = flagOr("--dedupe", "dedupe");  // hold coders whose work duplicates another's
    // The learning loop. It was implemented in Crew.cpp, advertised in the release
    // notes, and appended by the desktop's crew dialog — and NEVER PARSED HERE, so
    // the flag did nothing and the desktop checkbox was a no-op.
    o.learn = flagOr("--learn", "learn");
    o.security = hasFlag(args, "--security"); // read-only vulnerability scan (no code changes)
    o.swarmMax = flagValue(args, "--swarm", "0").toInt();  // raise the coder cap
    // N-sample self-consistency: N Director plans (keep the modal one) + an
    // N-reviewer audit panel that needs a strict majority to land anything.
    o.amplify = num("--amplify", "amplify", 1);
    o.land = hasFlag(args, "--review")
                 ? QStringLiteral("review")
                 : pack.value("land").toString(Config::str("crew.land", "auto"));
    o.coderBackend = str("--coder-backend", "coderBackend");
    o.coderModel = str("--coder-model", "coderModel");
    o.coderBackends = flagList(args, "--coder-backends");
    o.coderModels = flagList(args, "--coder-models");
    o.directorBackend = str("--director-backend", "directorBackend");
    o.directorModel = str("--director-model", "directorModel");
    o.auditorBackend = str("--auditor-backend", "auditorBackend");
    o.auditorModel = str("--auditor-model", "auditorModel");
    o.researcherBackend = str("--researcher-backend", "researcherBackend");
    o.researcherModel = str("--researcher-model", "researcherModel");

    if (o.task.isEmpty()) {
        err() << "crew needs a task: ollamadev crew \"build X\"\n";
        err().flush();
        return 2;
    }

    return runCrewAndReport(o);
}

// ----------------------------------------------- update / export / import

bool askYesNo(const QString& prompt);  // defined below, next to the other prompts

int cmdUpdate(const QStringList& args) {
    out() << "checking…\n";
    out().flush();
    const UpdateInfo u = Update::check();
    if (!u.ok) {
        err() << "could not check: " << u.error << "\n";
        err().flush();
        return 1;
    }
    if (!u.newer) {
        out() << "you are on " << u.current << " — that is the latest\n";
        out().flush();
        return 0;
    }

    out() << u.current << " → " << u.latest << "  (" << u.assetName << ", "
          << (u.assetSize / 1024 / 1024) << " MB)\n";
    if (!u.notes.trimmed().isEmpty()) out() << "\n" << u.notes.trimmed().left(600) << "\n\n";
    out() << "replaces: " << u.target << "\n";
    out().flush();

    // Dry by default. Overwriting the running binary is not a thing to do because
    // somebody typed a command that sounded like a question.
    if (!hasFlag(args, "--install")) {
        out() << "\ninstall it:  ollamadev update --install\n";
        out().flush();
        return 0;
    }
    if (!hasFlag(args, "--yes") && !askYesNo(QStringLiteral("replace it now?"))) {
        out() << "left alone\n";
        out().flush();
        return 0;
    }

    QString e;
    const bool done = Update::install(u, &e, [](qint64 got, qint64 total) {
        if (total <= 0) return;
        out() << "\r  " << (got * 100 / total) << "%   ";
        out().flush();
    });
    out() << "\n";
    out().flush();
    if (!done) {
        err() << "update failed: " << e << "\n";
        err().flush();
        return 1;
    }
    out() << "✓ now on " << u.latest << "\n";
    out().flush();
    return 0;
}

int cmdExport(const QStringList& args) {
    const QStringList pos = positionals(args);
    const bool all = hasFlag(args, "--all");

    QStringList ids;
    if (all) {
        for (const SessionMeta& m : Session::list()) ids << m.id;
    } else if (!pos.isEmpty()) {
        ids << pos.first();
    } else {
        const auto list = Session::list();  // newest first
        if (list.isEmpty()) {
            err() << "no sessions to export\n";
            err().flush();
            return 1;
        }
        ids << list.first().id;
    }

    QJsonArray sessions;
    for (const QString& id : ids) {
        const QJsonObject o = Session::exportOne(id);
        if (o.isEmpty()) {
            err() << "no session '" << id << "'\n";
            err().flush();
            return 1;
        }
        sessions.append(o);
    }

    // A self-describing bundle, not a bare session: `kind` and `version` are what
    // let import tell an OllamaDev export from any other JSON you point it at.
    const QJsonObject bundle{{"kind", "ollamadev-export"},
                             {"version", 1},
                             {"exported", QDateTime::currentSecsSinceEpoch()},
                             {"sessions", sessions}};

    QString path = flagValue(args, "--out");
    if (path.isEmpty())
        path = all ? QStringLiteral("ollamadev-export-all.json")
                   : QStringLiteral("ollamadev-export-%1.json").arg(ids.first());

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        err() << "cannot write " << path << "\n";
        err().flush();
        return 1;
    }
    f.write(QJsonDocument(bundle).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        err() << "cannot write " << path << "\n";
        err().flush();
        return 1;
    }
    out() << "✓ " << sessions.size() << " session(s) → " << path << "\n";
    out().flush();
    return 0;
}

int cmdImport(const QStringList& args) {
    const QString path = positionals(args).value(0);
    QFile f(path);
    if (path.isEmpty() || !f.open(QIODevice::ReadOnly)) {
        err() << "usage: ollamadev import <file.json>\n";
        err().flush();
        return 2;
    }
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.isEmpty()) {
        err() << "not JSON\n";
        err().flush();
        return 1;
    }

    // Take a bundle, or a single bare session — which is what a hand-copied session
    // file, or an export from the PHP app, looks like.
    QJsonArray sessions = o.value(QStringLiteral("sessions")).toArray();
    if (sessions.isEmpty() && o.contains(QStringLiteral("messages"))) sessions.append(o);
    if (sessions.isEmpty()) {
        err() << "no sessions in " << path << "\n";
        err().flush();
        return 1;
    }

    int n = 0;
    for (const QJsonValue& v : sessions) {
        QString e;
        // Always a NEW id: an import can never overwrite a session you already have,
        // so there is no collision to resolve.
        const QString id = Session::importOne(v.toObject(), QDir::currentPath(), &e);
        if (id.isEmpty()) {
            err() << "skipped one: " << e << "\n";
            err().flush();
            continue;
        }
        ++n;
        out() << "  " << id << "  "
              << v.toObject().value(QStringLiteral("messages")).toArray().size() << " messages\n";
    }
    out() << "✓ imported " << n << " session(s) — open one: ollamadev load <id>\n";
    out().flush();
    return n > 0 ? 0 : 1;
}


// ------------------------------------------------------------------------ tidy

int cmdTidy(const QStringList& args) {
    if (!GitFlow::isRepo()) {
        err() << "not a git repository\n";
        err().flush();
        return 1;
    }
    const int n = qBound(2, positionals(args).value(0, QStringLiteral("10")).toInt(), 50);

    RebasePlan plan = Rebase::planFor(n);
    if (plan.steps.isEmpty()) {
        out() << "nothing to tidy\n";
        out().flush();
        return 0;
    }

    const QString backend = Config::str("model.backend", "ollama");
    const QString model = GitFlow::modelFor(Config::str("ollama.defaultModel", ""));
    out() << "reading " << plan.steps.size() << " commits with " << model << "…\n";
    out().flush();

    CancelToken cancel;
    plan = Rebase::propose(plan, backend, model, cancel);

    if (!plan.rationale.isEmpty()) out() << "\n" << plan.rationale << "\n";
    out() << "\nthe plan (oldest first):\n";
    for (const RebaseStep& s : plan.steps) {
        out() << "  " << s.actionName().leftJustified(7) << s.sha.left(8) << "  " << s.subject
              << "\n";
        if (s.action == RebaseStep::Reword && !s.newMessage.isEmpty())
            out() << "          → " << s.newMessage.split(QLatin1Char('\n')).first() << "\n";
    }
    out() << "\n";
    out().flush();

    // Rewriting history is not something to do because a command sounded like a
    // question. --dry-run stops here; anything else asks.
    if (hasFlag(args, "--dry-run")) return 0;
    if (!hasFlag(args, "--yes") && !askYesNo(QStringLiteral("rewrite these commits?"))) {
        out() << "left alone\n";
        out().flush();
        return 0;
    }

    QString e;
    const RebaseResult r = Rebase::apply(plan, &e);
    if (!r.ok) {
        err() << (e.isEmpty() ? r.output : e) << "\n";
        if (r.conflicted) err() << "resolve it, or: git rebase --abort\n";
        err().flush();
        return 1;
    }
    out() << "✓ rewritten. undo with:  git reset --hard " << r.backupRef << "\n";
    out().flush();
    return 0;
}

// --------------------------------------------------------------------- plugins

int cmdPlugin(const QStringList& args) {
    const QString sub = args.value(0);

    if (sub.isEmpty() || sub == "list" || sub == "ls") {
        const auto list = Plugins::all();
        if (list.isEmpty()) {
            out() << "no plugins installed\n"
                     "  ollamadev plugin install <dir|https git url>\n\n"
                     "A plugin is a folder with a plugin.json. It can contribute skills,\n"
                     "slash-commands, hooks and MCP servers — the extension points that\n"
                     "already exist. It cannot run code of its own.\n";
            out().flush();
            return 0;
        }
        for (const Plugin& p : list) {
            out() << (p.enabled ? " ✓ " : " · ") << p.name.leftJustified(18) << "  "
                  << p.version.leftJustified(8) << "  " << p.description << "\n"
                  << "      " << p.capabilities() << "\n";
        }
        out() << "\n ✓ = enabled.  Enable one:  ollamadev plugin enable <name>\n";
        out().flush();
        return 0;
    }

    if (sub == "install") {
        const QString src = positionals(args).value(1);
        QString e, name;
        if (!Plugins::install(src, &e, &name)) {
            err() << "install failed: " << e << "\n";
            err().flush();
            return 1;
        }
        Plugin p;
        Plugins::get(name, &p);
        out() << "✓ installed " << name << "  [disabled]\n"
              << "  it provides: " << p.capabilities() << "\n"
              << "  nothing of it is live until:  ollamadev plugin enable " << name << "\n";
        out().flush();
        return 0;
    }

    if (sub == "enable") {
        const QString name = positionals(args).value(1);
        Plugin p;
        if (!Plugins::get(name, &p)) {
            err() << "no plugin '" << name << "'\n";
            err().flush();
            return 1;
        }
        // THE CONSENT MOMENT. Enabling is what turns a plugin's `hooks` into shell
        // commands that run on this machine, so print them IN FULL and ask. Nobody
        // can consent to something they were not shown.
        if (!p.hooks.isEmpty()) {
            out() << "\n" << p.name << " wants to run these commands on your machine:\n";
            for (const PluginHook& h : p.hooks)
                out() << "    on " << h.event
                      << (h.matcher.isEmpty() ? QString() : QStringLiteral(" [%1]").arg(h.matcher))
                      << ":  " << h.command << "\n";
            out() << "\n";
            out().flush();
            if (!hasFlag(args, "--yes") && !askYesNo(QStringLiteral("enable it?"))) {
                out() << "left disabled\n";
                out().flush();
                return 1;
            }
        }
        QString e;
        if (!Plugins::setEnabled(p.name, true, &e)) {
            err() << e << "\n";
            err().flush();
            return 1;
        }
        out() << "✓ enabled " << p.name << " — " << p.capabilities() << "\n";
        out().flush();
        return 0;
    }

    if (sub == "disable") {
        QString e;
        if (!Plugins::setEnabled(positionals(args).value(1), false, &e)) {
            err() << e << "\n";
            err().flush();
            return 1;
        }
        out() << "✓ disabled " << positionals(args).value(1) << "\n";
        out().flush();
        return 0;
    }

    if (sub == "remove" || sub == "rm" || sub == "uninstall") {
        QString e;
        if (!Plugins::remove(positionals(args).value(1), &e)) {
            err() << e << "\n";
            err().flush();
            return 1;
        }
        out() << "✓ removed " << positionals(args).value(1) << "\n";
        out().flush();
        return 0;
    }

    if (sub == "show") {
        Plugin p;
        if (!Plugins::get(positionals(args).value(1), &p)) {
            err() << "no plugin '" << positionals(args).value(1) << "'\n";
            err().flush();
            return 1;
        }
        out() << p.name << " " << p.version << (p.enabled ? "  [enabled]" : "  [disabled]") << "\n"
              << p.description << "\n"
              << (p.homepage.isEmpty() ? QString() : p.homepage + QStringLiteral("\n"))
              << "  dir:      " << p.dir << "\n"
              << "  provides: " << p.capabilities() << "\n";
        for (const PluginHook& h : p.hooks)
            out() << "  hook on " << h.event << ":  " << h.command << "\n";
        out().flush();
        return 0;
    }

    err() << "usage: ollamadev plugin [list | install <dir|https url> | enable <name> | disable "
             "<name> | remove <name> | show <name>]\n";
    err().flush();
    return 1;
}

// ------------------------------------------------------------------ workspaces

int cmdWorkspace(const QStringList& args) {
    const QString sub = args.value(0);

    if (sub.isEmpty() || sub == "list" || sub == "ls") {
        const auto list = Workspaces::all();
        if (list.isEmpty()) {
            out() << "no workspaces yet — bookmark this folder:\n  ollamadev ws add\n";
            out().flush();
            return 0;
        }
        const QString active = Workspaces::activeId();
        for (const Workspace& w : list) {
            out() << (w.id == active ? " * " : "   ") << w.name.leftJustified(20) << "  " << w.path
                  << "\n";
        }
        // A child process cannot change its parent's directory, so `open` PRINTS the
        // path and the shell does the cd. Say so, or the command looks broken.
        out() << "\n * = active.  Jump to one:  cd $(ollamadev ws open <name>)\n";
        out().flush();
        return 0;
    }

    if (sub == "add") {
        const QStringList pos = positionals(args);
        const Workspace w = Workspaces::add(pos.value(1), pos.value(2));
        out() << "✓ " << w.name << " → " << w.path << "\n";
        out().flush();
        return 0;
    }

    if (sub == "remove" || sub == "rm") {
        const QString key = positionals(args).value(1);
        if (key.isEmpty()) {
            err() << "usage: ollamadev ws rm <name|path|id>\n";
            err().flush();
            return 2;
        }
        if (!Workspaces::remove(key)) {
            err() << "no workspace '" << key << "'\n";
            err().flush();
            return 1;
        }
        out() << "✓ removed " << key << "\n";
        out().flush();
        return 0;
    }

    if (sub == "open") {
        const QString key = positionals(args).value(1);
        const QString path = Workspaces::open(key);
        if (path.isEmpty()) {
            // The path goes to STDOUT and nothing else does, because the whole
            // command is designed to be used as `cd $(…)`. An error on stdout would
            // be cd'd into.
            err() << "no workspace '" << key << "'\n";
            err().flush();
            return 1;
        }
        out() << path << "\n";
        out().flush();
        return 0;
    }

    err() << "usage: ollamadev ws [list | add [path] [name] | rm <name> | open <name>]\n";
    err().flush();
    return 1;
}

int cmdBoard(const QStringList& args) {
    const auto pend = Board::pending();
    if (hasFlag(args, "--json")) {
        QJsonArray a;
        for (const auto& d : pend)
            a.append(QJsonObject{{"id", d.id},
                                 {"kind", d.kind},
                                 {"summary", d.summary},
                                 {"data", d.data}});
        out() << QString::fromUtf8(json::encode(a)) << "\n";
        out().flush();
        return 0;
    }
    if (pend.isEmpty()) {
        out() << "nothing pending\n";
        out().flush();
        return 0;
    }
    for (const auto& d : pend) {
        out() << "  #" << d.data.value("n").toInt() << "  " << d.summary << "\n"
              << "      " << d.data.value("reason").toString() << "  ("
              << d.data.value("files").toArray().size() << " files)\n";
    }
    out().flush();
    return 0;
}

int cmdScan(const QStringList& args) {
    const QString path = positionals(args).value(0, QDir::currentPath());

    // scanTree, not scanFile. The default argument is the current DIRECTORY, and
    // scanFile only accepts a file — so `ollamadev scan`, the natural way to run
    // the scanner, used to examine NOTHING and print "clean". An all-clear from a
    // scan that never happened is worse than no scanner at all.
    int files = 0;
    const auto findings = SecScan::scanTree(path, &files);

    int high = 0;
    for (const auto& f : findings) {
        if (f.severity == "high") ++high;
        out() << "  " << f.severity.leftJustified(5) << " " << f.rule.leftJustified(16) << " "
              << (f.file.isEmpty() ? QString() : f.file + QLatin1Char(':')) << f.line << "  "
              << f.redacted << "\n";
    }

    // Always say what was actually looked at. "clean" on its own is a claim; "clean
    // — 0 files" is the truth about a scan that found nothing because it read
    // nothing, and the difference matters when the answer is about secrets.
    if (files == 0)
        out() << "nothing to scan in " << path << "\n";
    else if (findings.isEmpty())
        out() << "clean — " << files << " files scanned\n";
    else
        out() << "\n" << findings.size() << " finding(s) in " << files << " files scanned\n";

    out().flush();
    return high > 0 ? 1 : 0;
}

// `ollamadev agents [list|show <name>]` — the file-defined personas a subagent
// (the `task` tool) can adopt, from .ollamadev/agents/*.md (project shadows home).
int cmdAgents(const QStringList& args) {
    const QString sub = args.value(0);

    if (sub == "show") {
        const AgentDef d = AgentDefs::get(args.value(1));
        if (d.isNull()) {
            err() << "no agent '" << args.value(1) << "'\n";
            err().flush();
            return 1;
        }
        out() << "# " << d.name << "\n";
        if (!d.description.isEmpty()) out() << d.description << "\n";
        QStringList meta;
        if (!d.model.isEmpty()) meta << "model: " + d.model;
        if (!d.permission.isEmpty()) meta << "permission: " + d.permission;
        if (!d.tools.isEmpty()) meta << "tools: " + d.tools.join(", ");
        if (!meta.isEmpty()) out() << "(" << meta.join(" · ") << ")\n";
        out() << "\n" << d.prompt << "\n";
        out().flush();
        return 0;
    }

    const QVector<AgentDef> all = AgentDefs::all();
    if (all.isEmpty()) {
        out() << "no custom agents — create one at .ollamadev/agents/<name>.md\n"
                 "  (frontmatter: name, description, model, permission, tools; body = its system "
                 "prompt),\n"
                 "  then delegate to it: task(prompt, agent_type=\"<name>\").\n";
        out().flush();
        return 0;
    }
    out() << "Custom agents (" << all.size() << "):\n";
    for (const AgentDef& d : all) {
        out() << "  " << d.name.leftJustified(16) << "  "
              << (d.description.isEmpty() ? QStringLiteral("no description") : d.description);
        QStringList tags;
        if (!d.model.isEmpty()) tags << d.model;
        if (!d.permission.isEmpty()) tags << d.permission;
        if (!d.tools.isEmpty()) tags << QStringLiteral("%1 tools").arg(d.tools.size());
        if (!tags.isEmpty()) out() << "  [" << tags.join(", ") << "]";
        out() << "\n";
    }
    out().flush();
    return 0;
}

// Is the binary on the PATH the one that just ran? If not, every change the user
// makes appears to do nothing, and they debug the wrong file — a failure mode that
// wastes an afternoon and gives no clue that it is happening at all.
void reportStaleInstall() {
    const QString running = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("ollamadev"));
    if (onPath.isEmpty() || running.isEmpty()) return;

    const QString pathReal = QFileInfo(onPath).canonicalFilePath();
    if (pathReal == running) return;  // the usual, happy case

    const QDateTime mine = QFileInfo(running).lastModified();
    const QDateTime theirs = QFileInfo(pathReal).lastModified();
    out() << "binary      " << running << "\n";
    out() << "on PATH     " << pathReal
          << (theirs < mine ? "   ← OLDER than the one you are running" : "") << "\n";
    if (theirs < mine)
        out() << "            `ollamadev` in a shell runs THAT one. Refresh it: ./install.sh\n";
}

int cmdDoctor() {
    Config::load();
    out() << "OllamaDev " << ODV_VERSION << "\n";
    reportStaleInstall();
    out() << "config      " << Config::homeDir() << "\n";
    out() << "ollama host " << Config::str("ollama.host") << "\n";
    auto ollama = Backends::get("ollama");
    const bool up = ollama && ollama->available();
    out() << "ollama      " << (up ? "reachable" : "NOT reachable") << "\n";
    if (up) {
        const auto ms = ollama->models();
        out() << "models      " << ms.size() << " installed";
        const QString cloud = Models::firstCloud(ms);
        if (!cloud.isEmpty()) out() << "  (cloud: " << cloud << ")";
        out() << "\n";
    }
    out() << "CLIs        " << Backends::availableIds().join(", ") << "\n";
    out().flush();
    return up ? 0 : 1;
}

// `voice` records the mic and prints the transcript; `transcribe <file>` skips
// the recording. Both are 100% local — see core/Stt.h for the three engines.
int cmdVoice(const QStringList& args) {
    if (hasFlag(args, "--clear")) {
        Stt::clearHistory();
        out() << "✓ voice history cleared\n";
        out().flush();
        return 0;
    }
    if (hasFlag(args, "--history")) {
        const int n = flagValue(args, "--history", "10").toInt();
        const auto h = Stt::history(n > 0 ? n : 10);
        for (const auto& e : h) out() << "  " << e.value("text").toString() << "\n";
        if (h.isEmpty()) out() << "no voice history\n";
        out().flush();
        return 0;
    }

    const QString size = flagValue(args, "--model");
    if (!size.isEmpty()) Stt::setModelSize(size);

    QString e;
    if (hasFlag(args, "--setup")) {
        out() << "▸ provisioning whisper.cpp into " << Stt::sttDir() << "\n";
        out().flush();
        const bool ok = Stt::provision(
            [](const QString& label, qint64 done, qint64 total) {
                if (total <= 0) return;
                out() << QStringLiteral("\r  %1  %2%").arg(label).arg(done * 100 / total, 3);
                out().flush();
            },
            {}, &e);
        out() << "\n" << (ok ? "✓ ready" : "✗ " + e) << "\n";
        out().flush();
        return ok ? 0 : 1;
    }

    if (!Stt::Recorder::canRecord()) {
        err() << "no recorder — install alsa-utils, ffmpeg, or pulseaudio-utils\n";
        err().flush();
        return 1;
    }

    Stt::Recorder rec;
    if (!rec.start(&e)) {
        err() << "✗ " << e << "\n";
        err().flush();
        return 1;
    }

    out() << "recording (" << Stt::modelSize() << ") — press Enter to stop…";
    out().flush();
    QTextStream in(stdin);
    in.readLine();

    const QString wav = rec.stop();
    if (wav.isEmpty()) {
        err() << "✗ nothing captured\n";
        err().flush();
        return 1;
    }

    out() << "▸ transcribing…\n";
    out().flush();
    const QString text = Stt::transcribe(wav, &e);
    QFile::remove(wav);  // our temp file, by exact name

    if (text.isEmpty()) {
        err() << "✗ " << e << "\n";
        err().flush();
        return 1;
    }
    out() << text << "\n";
    out().flush();
    return 0;
}

int cmdTranscribe(const QStringList& args) {
    const QString path = args.value(0);
    if (path.isEmpty() || path.startsWith('-')) {
        err() << "usage: ollamadev transcribe <audio-file>\n";
        err().flush();
        return 2;
    }
    QString e;
    const QString text = Stt::transcribe(path, &e);
    if (text.isEmpty()) {
        err() << "✗ " << e << "\n";
        err().flush();
        return 1;
    }
    out() << text << "\n";
    out().flush();
    return 0;
}

int cmdOneShot(const QString& prompt, const QStringList& args) {
    const QString backend =
        flagValue(args, "--backend", Config::str("model.backend", "ollama"));
    QString model = flagValue(args, "--model", flagValue(args, "-m"));
    if (model.isEmpty()) {
        auto b = Backends::get(backend);
        model = b ? b->defaultModel() : QString();
    }

    // First-run dead end, turned into a signpost: on Ollama with nothing set up,
    // send the user to the one-step onboarding rather than a cryptic empty reply.
    if (backend == "ollama") {
        auto b = Backends::get(backend);
        if (b && b->available() && b->models().isEmpty()) {
            err() << "Ollama has no models yet. Run one-step setup (picks + pulls a model for "
                     "your hardware):\n  ollamadev setup\n";
            err().flush();
            return 1;
        }
        if (b && !b->available()) {
            err() << "Can't reach Ollama. Start it with `ollama serve`, then `ollamadev setup`.\n";
            err().flush();
            return 1;
        }
    }

    Agent a(backend, model);
    Permission::setMode(PermMode::Auto);
    Permission::setInteractive(true);
    Tools::setThreadRoot(QDir::currentPath());

    // A one-shot takes a prompt exactly like the REPL does, so it gets vision
    // exactly like the REPL does: `ollamadev "what is wrong with @screenshot.png"`
    // used to send the model the literal text "@screenshot.png" and nothing else.
    ChatMessage user;
    user.role = QStringLiteral("user");
    int attached = 0;
    user.content = Vision::attach(user, prompt, &attached);
    if (attached > 0) {
        err() << "attached " << attached << " image(s)\n";
        err().flush();
    }

    QVector<ChatMessage> msgs{
        {"system", a.buildSystemPrompt(QDir::currentPath()), {}, {}, {}, {}, {}}, user};

    StreamSink sink;
    sink.onContent = [](const QString& c) {
        out() << c;
        out().flush();
    };

    CancelToken cancel;

    // A model with no `tools` capability cannot run the agent loop at all — handed
    // a tool schema it replies with an empty string. Nearly every vision model is
    // in that boat, so `ollamadev -m moondream "what is in @shot.png"` printed
    // NOTHING: the image attached fine and the answer vanished. Have the
    // conversation instead — no tools, one turn — and say plainly that is what
    // happened. (This is not the banned text-protocol fallback: we are not scraping
    // prose for pretend tool calls, we are declining to offer tools to a model that
    // does not have them.)
    auto be = Backends::get(backend);
    if (be && be->supportsNativeTools() && !be->modelSupportsTools(model)) {
        err() << "note: " << model
              << " has no tool support — answering as a plain chat (it cannot edit files)\n";
        err().flush();
        const ChatTurn t = be->chat(model, msgs, QJsonArray(), sink, cancel);
        out() << "\n";
        out().flush();
        if (!t.ok || t.content.trimmed().isEmpty()) {
            err() << (t.error.isEmpty() ? QStringLiteral("the model returned nothing") : t.error)
                  << "\n";
            err().flush();
            return 1;
        }
        return 0;
    }

    const QString finalText =
        a.loop(msgs, Config::integer("agents.maxIterations", 20), sink, cancel);
    out() << "\n";
    out().flush();
    // The one-shot used to swallow the agent's error and exit 0 on an empty reply,
    // which is the worst way to fail: it reads as "the model had nothing to say".
    if (finalText.trimmed().isEmpty()) {
        err() << "the model returned nothing (run with a tool-capable model, or check `ollama "
                 "show "
              << model << " | grep capabilities`)\n";
        err().flush();
        return 1;
    }
    return 0;
}

// --------------------------------------------------------------- mcp / index

int cmdMcp(const QStringList& args) {
    const QString sub = args.value(0);

    // Must be first: everything below prints, and on the serve path stdout is the
    // JSON-RPC channel.
    if (sub == "serve") return McpServer::serve(hasFlag(args, "--allow-writes"));

    QString e;
    if (sub == "add") {
        if (!Mcp::addServer(args.value(1), args.value(2), args.mid(3), &e)) {
            err() << "✗ " << e << "\n";
            err().flush();
            return 1;
        }
        out() << "✓ added MCP server: " << args.value(1) << "\n";
        out().flush();
        return 0;
    }
    if (sub == "remove") {
        if (!Mcp::removeServer(args.value(1), &e)) {
            err() << "✗ " << e << "\n";
            err().flush();
            return 1;
        }
        out() << "✓ removed MCP server: " << args.value(1) << "\n";
        out().flush();
        return 0;
    }
    if (!sub.isEmpty() && sub != "list") {
        err() << "Usage: ollamadev mcp [list | add <name> <command> [args…] | remove <name> | "
                 "serve [--allow-writes]]\n";
        err().flush();
        return 2;
    }

    const auto servers = Mcp::servers();
    if (servers.isEmpty()) {
        out() << "MCP servers: (none configured)\n"
              << "  add one:  ollamadev mcp add <name> <command> [args…]\n";
        out().flush();
        return 0;
    }
    out() << "MCP servers:\n";
    for (const auto& s : servers) {
        const QString where = s.type == QLatin1String("stdio")
                                  ? (s.command + QLatin1Char(' ') + s.args.join(' ')).trimmed()
                                  : s.url;
        out() << "  " << s.name << "  " << where << (s.disabled ? "  (disabled)" : "") << "\n";
    }
    out().flush();
    return 0;
}

int cmdIndex(const QStringList& args) {
    const QString sub = args.value(0, QStringLiteral("status"));

    if (sub == "clear") {
        out() << (CodeIndex::clear() ? "✓ index cleared\n" : "✗ could not remove the index\n");
        out().flush();
        return 0;
    }

    if (sub == "build") {
        out() << "Indexing " << QDir::currentPath() << " with " << CodeIndex::model() << "…\n";
        out().flush();
        const BuildReport r = CodeIndex::build([](const QString& f, int done, int total) {
            out() << QStringLiteral("\r  %1/%2  %3").arg(done).arg(total).arg(f.left(50), -50);
            out().flush();
        });
        out() << "\r" << QString(70, ' ') << "\r";
        if (!r.ok) {
            err() << "✗ "
                  << (r.error == QLatin1String("embed_failed")
                          ? QStringLiteral("embedding failed — install the model first: ollama "
                                           "pull %1")
                                .arg(CodeIndex::model())
                          : r.error)
                  << "\n";
            err().flush();
            return 1;
        }
        out() << "✓ " << r.files << " files → " << r.chunks << " chunks";
        if (r.skipped > 0) out() << "  (" << r.skipped << " skipped)";
        out() << "\n";
        out().flush();
        return 0;
    }

    const IndexStatus s = CodeIndex::status();
    if (!s.exists) {
        out() << "no index yet — build it with: ollamadev index build\n";
        out().flush();
        return 1;
    }
    out() << "model   " << s.model << "\n"
          << "built   " << s.built << "\n"
          << "root    " << s.root << "\n"
          << "chunks  " << s.chunks << " over " << s.files << " files (dim " << s.dim << ")\n";
    out().flush();
    return 0;
}

int cmdCodeSearch(const QStringList& args) {
    const QString q = args.join(' ').trimmed();
    if (q.isEmpty()) {
        err() << "code-search needs a query: ollamadev code-search \"how does X work\"\n";
        err().flush();
        return 2;
    }
    const SearchReport r = CodeIndex::search(q, 8);
    if (!r.ok) {
        err() << "✗ "
              << (r.error == QLatin1String("no_index")
                      ? QStringLiteral("no index yet — build it with: ollamadev index build")
                      : QStringLiteral("embedding failed — is it installed? ollama pull %1")
                            .arg(CodeIndex::model()))
              << "\n";
        err().flush();
        return 1;
    }
    for (const IndexHit& h : r.hits) {
        out() << QStringLiteral("%1  %2:%3-%4\n")
                     .arg(h.score, 5, 'f', 3)
                     .arg(h.file)
                     .arg(h.start)
                     .arg(h.end);
    }
    out().flush();
    return 0;
}

// The CLI `search`. It calls WebSearch directly rather than Tools::run: the tool
// is mutates=true (network egress), and a non-interactive Ask-mode CLI would deny
// it — but the user typing `ollamadev search …` IS the approval.
int cmdSearch(const QStringList& args) {
    const QString q = args.join(' ').trimmed();
    if (q.isEmpty()) {
        err() << "search needs a query: ollamadev search \"qt6 forkpty\"\n";
        err().flush();
        return 2;
    }
    if (!WebSearch::webEnabled()) {
        err() << "web access is off (--no-web / web.enabled=false)\n";
        err().flush();
        return 1;
    }
    const SearchResult r = WebSearch::search(q, 5);
    if (!r.ok) {
        err() << "✗ search failed: " << r.error << "\n";
        err().flush();
        return 1;
    }
    int i = 0;
    for (const SearchHit& h : r.hits) {
        out() << ++i << ". " << h.title << "\n   " << h.url << "\n";
        if (!h.snippet.isEmpty()) out() << "   " << h.snippet << "\n";
        out() << "\n";
    }
    out().flush();
    return 0;
}

// ------------------------------------------------------------------- skills

int cmdSkills(const QStringList& args) {
    const QString sub = args.value(0);

    if (sub.isEmpty() || sub == "list") {
        for (const Skill& s : Skills::listForManager())
            out() << "  " << (s.builtin ? "·" : "✓") << " " << s.name.leftJustified(22) << "  "
                  << s.description << (s.builtin ? "  (built-in)" : "") << "\n";
        out() << "\n  ✓ installed   · built-in (add it to get an editable copy)\n";
        out().flush();
        return 0;
    }

    if (sub == "add") {
        const QString src = args.value(1);
        if (src.isEmpty()) {
            err() << "usage: ollamadev skills add <name|dir|git-url|archive> [--force]\n";
            err().flush();
            return 1;
        }
        const bool force = hasFlag(args, "--force");

        // A folder / git URL / archive is installed as-is. A bare name is a
        // registry entry if one matches, and otherwise a built-in — copying a
        // built-in onto disk is how you get an editable copy of it.
        SkillInstall r;
        const bool remote = src.contains("://") || src.startsWith("git@");
        const bool archive = src.endsWith(".zip") || src.endsWith(".tar.gz") ||
                             src.endsWith(".tgz");
        if (QFileInfo(src).isDir() || remote || archive) {
            r = Skills::install(src, force);
        } else {
            bool inRegistry = false;
            for (const Skill& s : Skills::browse())
                if (s.name.compare(src, Qt::CaseInsensitive) == 0) inRegistry = true;

            if (inRegistry) {
                r = Skills::addFromRegistry(src, force);
            } else {
                const Skill b = Skills::get(src);
                if (b.isNull()) {
                    err() << "no such skill, registry entry, or path: " << src << "\n";
                    err().flush();
                    return 1;
                }
                if (!b.dir.isEmpty() && !force) {
                    err() << src << " is already installed at " << b.dir << "\n";
                    err().flush();
                    return 1;
                }
                const QString slug = Skills::save(b.name, b.description, b.body);
                if (slug.isEmpty()) r.messages << QStringLiteral("could not write the skill");
                else r.installed << slug;
            }
        }

        for (const QString& m : r.messages) err() << "  " << m << "\n";
        for (const QString& n : r.installed) out() << "✓ installed " << n << "\n";
        out().flush();
        err().flush();
        return r.installed.isEmpty() ? 1 : 0;
    }

    if (sub == "new") {
        if (args.value(1).isEmpty()) {
            err() << "usage: ollamadev skills new <name>\n";
            err().flush();
            return 1;
        }
        out() << "✓ " << Skills::scaffold(args.value(1)) << "\n";
        out().flush();
        return 0;
    }

    if (sub == "show") {
        const Skill s = Skills::get(args.value(1));
        if (s.isNull()) {
            err() << "no skill named '" << args.value(1) << "'\n";
            err().flush();
            return 1;
        }
        out() << "# " << s.name << "\n" << s.description << "\n\n" << s.body << "\n";
        if (!s.files.isEmpty()) out() << "\nhelper files: " << s.files.join(", ") << "\n";
        out().flush();
        return 0;
    }

    if (sub == "search") {
        for (const Skill& s : Skills::search(args.mid(1).join(' ')))
            out() << "  " << (s.installed ? "✓" : "·") << " " << s.name.leftJustified(22) << "  "
                  << s.description << "\n";
        out().flush();
        return 0;
    }

    if (sub == "export") {
        const QString path = Skills::exportSkill(args.value(1), args.value(2));
        if (path.isEmpty()) {
            err() << "could not export '" << args.value(1) << "'\n";
            err().flush();
            return 1;
        }
        out() << "✓ " << path << "\n";
        out().flush();
        return 0;
    }

    if (sub == "rm" || sub == "remove") {
        if (!Skills::remove(args.value(1))) {
            err() << "no installed skill named '" << args.value(1) << "'\n";
            err().flush();
            return 1;
        }
        out() << "✓ removed " << args.value(1) << "\n";
        out().flush();
        return 0;
    }

    err() << "usage: ollamadev skills [list|add|new|show|search|export|rm]\n";
    err().flush();
    return 1;
}

// ------------------------------------------------------------------- memory

int cmdMemory(const QStringList& args) {
    const QString sub = args.value(0);

    if (sub.isEmpty() || sub == "list") {
        const auto notes = Memory::all();
        if (notes.isEmpty()) {
            out() << "memory is empty — write one with: "
                     "ollamadev memory new \"<title>\" \"<body>\"\n";
            out().flush();
            return 0;
        }
        for (const MemoryNote& m : notes) {
            out() << "  " << m.slug.leftJustified(28) << "  " << m.title;
            if (!m.tags.isEmpty()) out() << "  [" << m.tags.join(", ") << "]";
            if (!m.links.isEmpty()) out() << "  → " << m.links.join(", ");
            out() << "\n";
        }
        out().flush();
        return 0;
    }

    if (sub == "new" || sub == "add") {
        const QString title = args.value(1);
        if (title.isEmpty()) {
            err() << "usage: ollamadev memory new \"<title>\" [\"<body>\"] [--tags a,b]\n";
            err().flush();
            return 1;
        }
        const QStringList tags = flagList(args, "--tags");
        QStringList rest = args.mid(2);
        const int t = rest.indexOf("--tags");
        if (t >= 0) rest = rest.mid(0, t);

        const QString body = rest.join(' ');
        const QString slug = Memory::save(
            title, body.isEmpty() ? QStringLiteral("(no body yet)") : body, tags);
        out() << "✓ saved " << slug << " → " << Memory::projectDir() << "/" << slug << ".md\n";
        out().flush();
        return 0;
    }

    if (sub == "show" || sub == "get") {
        const MemoryNote m = Memory::get(args.value(1));
        if (m.isNull()) {
            err() << "no memory '" << args.value(1) << "'\n";
            err().flush();
            return 1;
        }
        out() << "# " << m.title << "  (" << m.slug << ")\n";
        if (!m.tags.isEmpty()) out() << "tags: " << m.tags.join(", ") << "\n";
        out() << "\n" << m.body.trimmed() << "\n";
        if (!m.links.isEmpty()) out() << "\nlinks: " << m.links.join(", ") << "\n";
        out().flush();
        return 0;
    }

    if (sub == "search") {
        const auto hits = Memory::search(args.mid(1).join(' '));
        for (const MemoryNote& m : hits)
            out() << "  " << m.slug.leftJustified(28) << "  " << m.title << "\n";
        if (hits.isEmpty()) out() << "no matches\n";
        out().flush();
        return 0;
    }

    if (sub == "graph") {
        const QJsonObject g = Memory::graph();
        if (hasFlag(args, "--json")) {
            out() << QString::fromUtf8(QJsonDocument(g).toJson(QJsonDocument::Indented));
            out().flush();
            return 0;
        }
        const QJsonArray nodes = g.value("nodes").toArray();
        const QJsonArray edges = g.value("edges").toArray();
        out() << nodes.size() << " notes, " << edges.size() << " links\n";
        for (const QJsonValue& v : nodes) {
            const QJsonObject n = v.toObject();
            out() << "  " << n.value("id").toString().leftJustified(28) << "  degree "
                  << n.value("degree").toInt() << "\n";
        }
        for (const QJsonValue& v : edges) {
            const QJsonObject e = v.toObject();
            out() << "  " << e.value("from").toString() << " → " << e.value("to").toString()
                  << "\n";
        }
        out().flush();
        return 0;
    }

    if (sub == "rm" || sub == "remove") {
        if (!Memory::remove(args.value(1))) {
            err() << "no memory '" << args.value(1) << "'\n";
            err().flush();
            return 1;
        }
        out() << "✓ removed " << args.value(1) << "\n";
        out().flush();
        return 0;
    }

    err() << "usage: ollamadev memory [list|new|show|search|graph|rm]\n";
    err().flush();
    return 1;
}

// ------------------------------------------------------------ crew role / pack

int cmdCrewRole(const QStringList& args) {
    const QString sub = args.value(0);

    if (sub.isEmpty() || sub == "list") {
        for (const CrewRole& r : CrewRoles::all()) {
            out() << "  " << r.name.leftJustified(14) << "  " << r.desc;
            if (!r.model.isEmpty()) out() << "  [" << r.model << "]";
            if (r.readOnly) out() << "  (read-only)";
            if (r.custom) out() << "  (custom)";
            out() << "\n";
        }
        out().flush();
        return 0;
    }

    if (sub == "add") {
        const QString name = args.value(1);
        const QString prompt = args.value(2);
        if (name.isEmpty() || prompt.isEmpty()) {
            err() << "usage: ollamadev crew role add <name> \"<persona prompt>\" "
                     "[--desc \"…\"] [--model <m>] [--readonly]\n";
            err().flush();
            return 1;
        }
        const QString path =
            CrewRoles::add(name, prompt, flagValue(args, "--desc"), flagValue(args, "--model"),
                           hasFlag(args, "--readonly"));
        if (path.isEmpty()) {
            err() << "could not write the role\n";
            err().flush();
            return 1;
        }
        out() << "✓ " << path << "\n";
        out().flush();
        return 0;
    }

    if (sub == "show") {
        const CrewRole r = CrewRoles::get(args.value(1));
        out() << "# " << r.name << "\n" << r.desc << "\n\n" << r.prompt << "\n";
        if (!r.model.isEmpty()) out() << "\nmodel: " << r.model << "\n";
        out() << "permission: " << (r.readOnly ? "readonly" : "auto") << "\n";
        out().flush();
        return 0;
    }

    if (sub == "rm" || sub == "remove") {
        if (!CrewRoles::remove(args.value(1))) {
            err() << "no custom role '" << args.value(1) << "' (built-ins have no file)\n";
            err().flush();
            return 1;
        }
        out() << "✓ removed " << args.value(1) << "\n";
        out().flush();
        return 0;
    }

    err() << "usage: ollamadev crew role [list|add|show|rm]\n";
    err().flush();
    return 1;
}

int cmdCrewPack(const QStringList& args) {
    const QString sub = args.value(0);

    if (sub.isEmpty() || sub == "list") {
        for (const auto& p : CrewPacks::all())
            out() << "  " << p.first.leftJustified(16) << "  " << p.second << "\n";
        out().flush();
        return 0;
    }

    if (sub == "save") {
        const QString name = args.value(1);
        if (name.isEmpty()) {
            err() << "usage: ollamadev crew pack save <name> [--focus \"…\"] [--coder-model <m>] "
                     "[--coder-backend <b>] [--max <n>] [--amplify <n>] [--land auto|review]\n";
            err().flush();
            return 1;
        }
        // Only the reusable knobs of a team — never the one-off task.
        QJsonObject pack;
        const struct {
            const char* flag;
            const char* key;
        } strKeys[] = {{"--focus", "focus"},
                       {"--director-model", "directorModel"},
                       {"--coder-model", "coderModel"},
                       {"--auditor-model", "auditorModel"},
                       {"--researcher-model", "researcherModel"},
                       {"--director-backend", "directorBackend"},
                       {"--coder-backend", "coderBackend"},
                       {"--auditor-backend", "auditorBackend"},
                       {"--researcher-backend", "researcherBackend"},
                       {"--land", "land"},
                       {"--skills", "skills"},
                       {"--hosts", "hosts"}};
        for (const auto& k : strKeys) {
            const QString v = flagValue(args, k.flag);
            if (!v.isEmpty()) pack.insert(k.key, v);
        }
        const QString max = flagValue(args, "--max");
        if (!max.isEmpty()) pack.insert("max", max.toInt());
        const QString amp = flagValue(args, "--amplify");
        if (!amp.isEmpty()) pack.insert("amplify", amp.toInt());
        if (hasFlag(args, "--no-research")) pack.insert("research", false);
        if (hasFlag(args, "--no-audit")) pack.insert("audit", false);
        // The brain belongs to the team too — a "hard backend work" pack that
        // routes and debates is exactly the thing you get tired of retyping.
        if (hasFlag(args, "--route")) pack.insert("route", true);
        if (hasFlag(args, "--debate")) pack.insert("debate", true);
        if (hasFlag(args, "--dedupe")) pack.insert("dedupe", true);
        if (hasFlag(args, "--learn")) pack.insert("learn", true);

        const QString path = CrewPacks::save(name, pack);
        if (path.isEmpty()) {
            err() << "could not write the pack\n";
            err().flush();
            return 1;
        }
        out() << "✓ " << path << "\n";
        out().flush();
        return 0;
    }

    if (sub == "show") {
        const QJsonObject p = CrewPacks::load(args.value(1));
        if (p.isEmpty()) {
            err() << "no pack '" << args.value(1) << "'\n";
            err().flush();
            return 1;
        }
        out() << QString::fromUtf8(QJsonDocument(p).toJson(QJsonDocument::Indented));
        out().flush();
        return 0;
    }

    if (sub == "rm" || sub == "remove") {
        if (!CrewPacks::remove(args.value(1))) {
            err() << "no saved pack '" << args.value(1) << "' (built-ins have no file)\n";
            err().flush();
            return 1;
        }
        out() << "✓ removed " << args.value(1) << "\n";
        out().flush();
        return 0;
    }

    err() << "usage: ollamadev crew pack [list|save|show|rm]\n";
    err().flush();
    return 1;
}

// ---------------------------------------------------------------------------
// Git workflow (GitFlow) and test/verify (Verify).
// ---------------------------------------------------------------------------

// The backend + model the AI git commands run on. GitFlow::modelFor() then gives
// the dedicated `git.model` key the last word, so commits can run on a small fast
// model while chat stays on a big one.
QString gitBackend(const QStringList& args) {
    return flagValue(args, "--backend", Config::str("model.backend", "ollama"));
}
QString gitModel(const QStringList& args) {
    const QString m = flagValue(args, "--model", flagValue(args, "-m"));
    if (!m.isEmpty()) return m;
    auto b = Backends::get(gitBackend(args));
    return b ? b->defaultModel() : QString();
}

// stdin y/N. Core owns no terminal, so the confirmation the commit/push path asks
// for is answered here.
bool askYesNo(const QString& prompt) {
    out() << prompt << " [y/N] ";
    out().flush();
    QTextStream in(stdin);
    const QString a = in.readLine().trimmed().toLower();
    return a == "y" || a == "yes";
}

void printFindings(const QVector<Finding>& findings) {
    for (const auto& f : findings)
        err() << "  " << f.severity << "  " << f.rule << "  line " << f.line << "  " << f.redacted
              << "\n";
}

int cmdDiff(const QStringList& args) {
    if (!GitFlow::isRepo()) {
        err() << "not a git repository\n";
        err().flush();
        return 1;
    }
    const QString diff = GitFlow::workingDiff();

    if (hasFlag(args, "--json")) {
        QJsonArray findings;
        for (const auto& f : SecScan::scanDiff(diff)) {
            findings.append(QJsonObject{{"rule", f.rule},
                                        {"severity", f.severity},
                                        {"line", f.line},
                                        {"redacted", f.redacted}});
        }
        const QJsonObject o{
            {"branch", GitFlow::branch()}, {"diff", diff}, {"findings", findings}};
        out() << QString::fromUtf8(json::encode(o)) << "\n";
        out().flush();
        return 0;
    }

    if (diff.trimmed().isEmpty()) {
        out() << "no changes\n";
        out().flush();
        return 0;
    }
    for (const QString& line : diff.split('\n')) {
        // +++/--- are headers, not content; colouring them as add/remove makes every
        // hunk look like it both added and deleted a file.
        if (line.startsWith("+++") || line.startsWith("---"))
            out() << "\033[1m" << line << "\033[0m\n";
        else if (line.startsWith('+'))
            out() << "\033[32m" << line << "\033[0m\n";
        else if (line.startsWith('-'))
            out() << "\033[31m" << line << "\033[0m\n";
        else if (line.startsWith("@@"))
            out() << "\033[36m" << line << "\033[0m\n";
        else
            out() << line << "\n";
    }
    out().flush();
    return 0;
}

// Shared reporting for commit/ship, so a blocked commit reads the same either way.
int reportCommit(const CommitResult& r) {
    if (r.blocked) {
        err() << "\n\033[31m✗ blocked: " << r.error << "\033[0m\n";
        printFindings(r.findings);
        err() << "\nFix them, or commit anyway with --force.\n";
        err().flush();
        return 1;
    }
    if (!r.ok) {
        err() << "✗ " << r.error << "\n";
        err().flush();
        return 1;
    }
    out() << "\033[32m✓ committed\033[0m " << r.sha << "\n\n" << r.message << "\n";
    out().flush();
    return 0;
}

// Both commit and ship print a progress line before the model is asked, so the
// "not a repo" case has to be caught here — otherwise we announce that we are
// writing a commit message for a directory that cannot hold a commit.
bool requireRepo() {
    if (GitFlow::isRepo()) return true;
    err() << "not a git repository\n";
    err().flush();
    return false;
}

int cmdCommit(const QStringList& args) {
    if (!requireRepo()) return 1;
    CommitOptions o;
    o.stageAll = hasFlag(args, "-a");
    o.message = flagValue(args, "-m");
    o.force = hasFlag(args, "--force");
    o.backendId = gitBackend(args);
    o.model = gitModel(args);

    if (o.message.isEmpty()) {
        out() << "\033[2mwriting a commit message…\033[0m\n";
        out().flush();
    }
    // No Confirm: plain `commit` is not a remote-visible step, so it just commits.
    return reportCommit(GitFlow::commit(o, {}, CancelToken{}));
}

int cmdShip(const QStringList& args) {
    if (!requireRepo()) return 1;
    CommitOptions o;
    o.stageAll = true;  // ship means "everything I have"
    o.message = flagValue(args, "-m");
    o.force = hasFlag(args, "--force");
    o.assumeYes = hasFlag(args, "--yes");
    o.askBeforeCommit = true;
    o.backendId = gitBackend(args);
    o.model = gitModel(args);

    if (o.message.isEmpty()) {
        out() << "\033[2mstaging, scanning, writing a commit message…\033[0m\n";
        out().flush();
    }

    // --yes answers the QUESTIONS (commit, push) so CI can run unattended. It never
    // answers the secret gate — that is decided inside GitFlow::commit() before this
    // is ever called, and only --force opens it.
    const ShipResult r = GitFlow::ship(
        o, [&o](const QString& prompt) { return o.assumeYes ? true : askYesNo(prompt); },
        CancelToken{});

    if (!r.commit.ok) return reportCommit(r.commit);
    reportCommit(r.commit);
    if (r.pushed) {
        out() << "\033[32m✓ pushed\033[0m\n";
        out().flush();
        return 0;
    }
    err() << "\033[33m" << r.error << "\033[0m\n";
    err().flush();
    return r.error == "committed, not pushed" ? 0 : 1;
}

int cmdPr(const QStringList& args) {
    const QString sub = args.value(0);
    if (!GitFlow::isRepo()) {
        err() << "not a git repository\n";
        err().flush();
        return 1;
    }
    if (!GitFlow::hasGh()) {
        err() << "`pr` needs the GitHub CLI. Install `gh` and run `gh auth login`.\n"
                 "Everything else (diff, commit, ship, git) works without it.\n";
        err().flush();
        return 1;
    }

    if (sub == "create") {
        QString base = flagValue(args, "--base");
        if (base.isEmpty()) {
            // The remote's default branch, if git knows it; otherwise let the user say.
            const GitResult r = GitFlow::git(
                {"rev-parse", "--abbrev-ref", "--verify", "origin/HEAD"});
            base = r.ok() ? r.output.trimmed().section('/', 1) : QStringLiteral("main");
        }
        const QString range = base + "..HEAD";
        const QString commits = GitFlow::git({"--no-pager", "log", "--oneline", range}).output;
        const QString diff = GitFlow::git({"--no-pager", "diff", base + "...HEAD"}).output;
        if (commits.trimmed().isEmpty()) {
            err() << "no commits on this branch vs " << base << "\n";
            err().flush();
            return 1;
        }

        out() << "\033[2mdrafting the PR…\033[0m\n";
        out().flush();
        QString title, body;
        GitFlow::prText(commits, diff, gitBackend(args), GitFlow::modelFor(gitModel(args)), &title,
                        &body, CancelToken{});

        out() << "\n\033[1m" << title << "\033[0m\n\n" << body << "\n\n";
        out().flush();
        if (!askYesNo("Open this PR?")) {
            out() << "cancelled\n";
            out().flush();
            return 0;
        }

        // Flags verified against `gh pr create --help`: -B/--base, -t/--title,
        // -F/--body-file with "-" reading the body from stdin (a PR body is markdown
        // with newlines — argv is the wrong place for it).
        QProcess gh;
        gh.setProgram("gh");
        gh.setArguments({"pr", "create", "--base", base, "--title", title, "--body-file", "-"});
        gh.setWorkingDirectory(Tools::threadRoot());
        gh.setProcessChannelMode(QProcess::MergedChannels);
        gh.start();
        if (!gh.waitForStarted(10000)) {
            err() << "could not run gh\n";
            err().flush();
            return 1;
        }
        gh.write(body.toUtf8());
        gh.closeWriteChannel();
        gh.waitForFinished(120000);
        out() << QString::fromUtf8(gh.readAll());
        out().flush();
        return gh.exitCode();
    }

    if (sub == "review") {
        const QString n = args.value(1);
        if (n.isEmpty()) {
            err() << "usage: ollamadev pr review <n> [--comment]\n";
            err().flush();
            return 1;
        }
        QProcess diffProc;
        diffProc.setProgram("gh");
        diffProc.setArguments({"pr", "diff", n});
        diffProc.setWorkingDirectory(Tools::threadRoot());
        diffProc.start();
        diffProc.waitForFinished(120000);
        const QString diff = QString::fromUtf8(diffProc.readAllStandardOutput());
        if (diff.trimmed().isEmpty()) {
            err() << "could not fetch the diff for PR #" << n << "\n";
            err().flush();
            return 1;
        }

        out() << "\033[2mreviewing PR #" << n << "…\033[0m\n";
        out().flush();
        const PrReview rv =
            GitFlow::review(diff, gitBackend(args), GitFlow::modelFor(gitModel(args)),
                            CancelToken{});
        if (rv.verdict.isEmpty()) {
            err() << "review unavailable (could not parse the model response)\n";
            err().flush();
            return 1;
        }

        QString report = "Verdict: " + rv.verdict.toUpper();
        if (!rv.summary.isEmpty()) report += " — " + rv.summary;
        report += "\n";
        if (rv.findings.isEmpty())
            report += "\nNo blocking findings.\n";
        else {
            report += "\nFindings:\n";
            for (const QString& f : rv.findings) report += "  - " + f + "\n";
        }
        out() << "\n" << report;
        out().flush();

        if (hasFlag(args, "--comment")) {
            // `gh pr comment -F -` reads the body from stdin — same reason as create.
            QProcess c;
            c.setProgram("gh");
            c.setArguments({"pr", "comment", n, "--body-file", "-"});
            c.setWorkingDirectory(Tools::threadRoot());
            c.setProcessChannelMode(QProcess::MergedChannels);
            c.start();
            if (!c.waitForStarted(10000)) {
                err() << "could not run gh\n";
                err().flush();
                return 1;
            }
            c.write(report.toUtf8());
            c.closeWriteChannel();
            c.waitForFinished(120000);
            out() << QString::fromUtf8(c.readAll());
            out().flush();
            return c.exitCode();
        }
        return 0;
    }

    err() << "usage: ollamadev pr create [--base <branch>] | ollamadev pr review <n> [--comment]\n";
    err().flush();
    return 1;
}

int cmdGit(const QStringList& args) {
    // An allowlist, not a passthrough: `ollamadev git` is a convenience over the
    // porcelain, and forwarding arbitrary subcommands would quietly make every
    // destructive one (reset --hard, clean -fdx) reachable through the agent's CLI.
    static const QStringList allowed{"status", "diff",   "log",  "branch", "checkout", "commit",
                                     "add",    "push",   "pull", "stash",  "show"};
    const QString sub = args.value(0);
    if (sub.isEmpty() || !allowed.contains(sub)) {
        err() << "usage: ollamadev git <" << allowed.join('|') << ">\n";
        err().flush();
        return 1;
    }
    if (!GitFlow::isRepo()) {
        err() << "not a git repository\n";
        err().flush();
        return 1;
    }
    const GitResult r = GitFlow::git(QStringList{"--no-pager"} + args);
    out() << r.output;
    out().flush();
    return r.exit;
}

// Detect once, and say so — a wrong guess should be visible and fixable with
// `test.command`, not silently run.
std::optional<TestCommand> detectTests() {
    const auto t = Verify::detect(QDir::currentPath());
    if (!t) {
        err() << "could not detect a test command.\n"
                 "Set one explicitly:  ollamadev config set test.command \"<cmd>\"\n";
        err().flush();
    }
    return t;
}

int cmdTest(const QStringList&) {
    const auto t = detectTests();
    if (!t) return 1;

    out() << "\033[2m[" << t->label << "] " << t->cmd << "\033[0m\n";
    out().flush();
    const TestRun r = Verify::run(*t,
                                  [](const QString& chunk) {
                                      out() << chunk;
                                      out().flush();
                                  },
                                  CancelToken{});
    out() << (r.green() ? "\n\033[32m✓ tests pass\033[0m\n" : "\n\033[31m✗ tests failed\033[0m\n");
    out().flush();
    return r.exit;
}

int cmdVerify(const QStringList& args) {
    const auto t = detectTests();
    if (!t) return 1;

    const int max = flagValue(args, "--max", "3").toInt();
    const QString backend = gitBackend(args);
    const QString model = gitModel(args);

    // The fix agent edits files, so it gets the project as its root. Explicitly, not
    // by falling back to the process cwd — that is what confines its writes.
    Tools::setThreadRoot(QDir::currentPath());

    out() << "\033[2m[" << t->label << "] " << t->cmd << "  (up to " << max
          << " fix attempt(s))\033[0m\n";
    out().flush();

    VerifyEvents ev;
    ev.onOutput = [](const QString& chunk) {
        out() << chunk;
        out().flush();
    };
    ev.onAttempt = [max](int attempt, int, bool green) {
        if (green)
            out() << "\n\033[32m✓ tests pass\033[0m"
                  << (attempt > 1 ? QStringLiteral(" after %1 fix attempt(s)").arg(attempt - 1)
                                  : QString())
                  << "\n";
        else
            out() << "\n\033[33m✗ attempt " << attempt << "/" << max << ": failing\033[0m\n";
        out().flush();
    };
    ev.onFixStart = [] {
        out() << "\033[2m  asking the agent to fix…\033[0m ";
        out().flush();
    };
    ev.onFixStep = [] {
        out() << "\033[2m·\033[0m";
        out().flush();
    };

    const int code = Verify::fixLoop(*t, max, backend, model, ev, CancelToken{});
    if (code != 0) {
        err() << "\n\033[33mStill failing after " << max
              << " attempt(s).\033[0m Review the changes before committing.\n";
        err().flush();
    }
    return code;
}

// ---------------------------------------------------------------- terminal

int cmdTerminal(const QStringList& args) {
    const QString sub = args.value(0);

    // The host is not a user-facing verb: it is what `terminal start` re-executes
    // this binary as, and it blocks forever owning the pty. Kept as a subcommand so
    // the host is OUR OWN binary (applicationFilePath), never a PATH lookup.
    if (sub == "__host__") return Terminals::hostMain(args.value(1));

    QString e;

    if (sub == "create" || sub == "spawn") {
        const QString id = args.value(1);
        if (id.isEmpty()) {
            err() << "usage: ollamadev terminal " << sub
                  << " <name>" << (sub == "spawn" ? " <command…>" : "") << " [--cwd <dir>]\n";
            err().flush();
            return 2;
        }
        const QString cwd = flagValue(args, "--cwd", QDir::currentPath());
        const QString model = flagValue(args, "--model", flagValue(args, "-m"));

        // Everything after the name that is not a flag or a flag's value is the
        // command to run (spawn only).
        QStringList command;
        if (sub == "spawn") {
            static const QStringList takesValue{"--cwd", "--model", "-m"};
            for (int i = 2; i < args.size(); ++i) {
                if (args.at(i).startsWith('-')) {
                    if (takesValue.contains(args.at(i))) ++i;
                    continue;
                }
                command << args.at(i);
            }
            if (command.isEmpty()) {
                err() << "usage: ollamadev terminal spawn <name> <command…>\n";
                err().flush();
                return 2;
            }
        }

        const TerminalInfo t = Terminals::spawn(id, command, cwd, model, &e);
        if (t.isNull()) {
            err() << "✗ " << e << "\n";
            err().flush();
            return 1;
        }
        out() << "✓ " << t.id << " running (host pid " << t.hostPid << ", shell pid " << t.shellPid
              << ")\n"
              << "  attach: ollamadev terminal attach " << t.id << "\n";
        out().flush();
        return 0;
    }

    if (sub.isEmpty() || sub == "list" || sub == "ls") {
        const auto all = Terminals::list();
        if (all.isEmpty()) {
            out() << "no terminals — create one: ollamadev terminal create <name>\n";
            out().flush();
            return 0;
        }
        for (const TerminalInfo& t : all) {
            out() << "  " << (t.running ? "●" : "○") << " " << t.id.leftJustified(16) << "  "
                  << (t.running ? QStringLiteral("running  pid %1").arg(t.hostPid)
                                : QStringLiteral("stopped        "))
                  << "  " << t.cwd << "\n";
        }
        out().flush();
        return 0;
    }

    if (sub == "start" || sub == "stop" || sub == "delete" || sub == "rm") {
        const QString id = args.value(1);
        if (id.isEmpty()) {
            err() << "usage: ollamadev terminal " << sub << " <name>\n";
            err().flush();
            return 2;
        }
        const bool ok = sub == "start"  ? Terminals::start(id, &e)
                        : sub == "stop" ? Terminals::stop(id, &e)
                                        : Terminals::remove(id, &e);
        if (!ok) {
            err() << "✗ " << e << "\n";
            err().flush();
            return 1;
        }
        out() << "✓ " << sub << " " << id << "\n";
        out().flush();
        return 0;
    }

    if (sub == "log") {
        const QString id = args.value(1);
        if (id.isEmpty() || !Terminals::exists(id)) {
            err() << "usage: ollamadev terminal log <name> [-n <lines>]\n";
            err().flush();
            return 2;
        }
        const int n = flagValue(args, "-n", "100").toInt();
        out() << Terminals::log(id, n > 0 ? n : 100) << "\n";
        out().flush();
        return 0;
    }

    if (sub == "attach") {
        const int code = Terminals::attach(args.value(1), &e);
        if (!e.isEmpty()) {
            err() << "✗ " << e << "\n";
            err().flush();
            return 1;
        }
        return code;
    }

    if (sub == "broadcast" || sub == "send") {
        // `send <id> <text>` types into one terminal; `broadcast <text>` into all.
        const bool one = sub == "send";
        const QString id = one ? args.value(1) : QString();
        const QString text = args.mid(one ? 2 : 1).join(' ');
        if (text.isEmpty() || (one && id.isEmpty())) {
            err() << "usage: ollamadev terminal " << sub << (one ? " <name>" : "") << " \"<text>\"\n";
            err().flush();
            return 2;
        }
        const QByteArray keys = text.toUtf8() + '\n';  // a typed line ends in Return
        if (one) {
            if (!Terminals::send(id, keys, &e)) {
                err() << "✗ " << e << "\n";
                err().flush();
                return 1;
            }
            out() << "✓ sent to " << id << "\n";
            out().flush();
            return 0;
        }
        const int n = Terminals::broadcast(keys);
        out() << "✓ sent to " << n << " terminal(s)\n";
        out().flush();
        return n > 0 ? 0 : 1;
    }

    err() << "usage: ollamadev terminal [create|spawn|list|start|stop|attach|send|broadcast|"
             "delete|log] <name>\n";
    err().flush();
    return 2;
}

// -------------------------------------------------------------------- watch

int cmdWatch(const QStringList& args) {
    WatchOptions o;
    o.intervalSec = flagValue(args, "--interval", "2").toInt();
    o.once = hasFlag(args, "--once");
    o.iterations = Config::integer("watch.iterations", 8);
    o.backend = flagValue(args, "--backend");
    o.model = flagValue(args, "--model", flagValue(args, "-m"));

    // The task is the first positional; the rest are paths to watch. --interval
    // takes a value, so its number must not be mistaken for either.
    static const QStringList takesValue{"--interval", "--backend", "--model", "-m"};
    QStringList words;
    for (int i = 0; i < args.size(); ++i) {
        if (args.at(i).startsWith('-')) {
            if (takesValue.contains(args.at(i))) ++i;
            continue;
        }
        words << args.at(i);
    }
    o.task = words.value(0);
    o.paths = words.mid(1);
    return Watch::run(o);
}

// -------------------------------------------------------------------- hooks

int cmdHooks(const QStringList& args) {
    out() << Hooks::editorCommand(args);
    out().flush();
    return 0;
}

// User-defined slash commands: `ollamadev commands` lists them, `/<name> [args]`
// expands the template and runs it as a one-shot.
int cmdCommands(const QStringList& args) {
    const QString sub = args.value(0);
    if (sub == "show") {
        const QString body = UserCmds::expand(args.value(1), args.mid(2).join(' '));
        if (body.isEmpty()) {
            err() << "no custom command '" << args.value(1) << "'\n";
            err().flush();
            return 1;
        }
        out() << body << "\n";
        out().flush();
        return 0;
    }
    out() << UserCmds::render();
    out().flush();
    return 0;
}

// ------------------------------------------------------------------- lsp / eval

int cmdLsp(const QStringList& args) {
    // NOTHING may be printed on the way in: on the stdio path stdout is the LSP
    // channel from the moment serve() takes fd 1, and a banner here would be the
    // first thing the editor's framing parser choked on. Status goes to stderr,
    // inside serve().
    LspOptions o;
    o.port = flagValue(args, "--port", "0").toInt();
    o.backend = flagValue(args, "--backend");
    o.model = flagValue(args, "--model", flagValue(args, "-m"));
    return LspServer::serve(o);
}

int cmdEval(const QStringList& args) {
    EvalOptions o;
    o.only = flagValue(args, "--only");
    o.backend = flagValue(args, "--backend", Config::str("model.backend", "ollama"));
    o.model = flagValue(args, "--model", flagValue(args, "-m"));
    o.json = hasFlag(args, "--json");
    o.keep = hasFlag(args, "--keep");
    o.min = flagValue(args, "--min", "-1").toDouble();

    QStringList models;
    for (const QString& m : flagValue(args, "--compare").split(',', Qt::SkipEmptyParts))
        models << m.trimmed();
    if (models.isEmpty()) {
        QString m = o.model;
        if (m.isEmpty()) {
            auto b = Backends::get(o.backend);
            m = b ? b->defaultModel() : QString();
        }
        if (m.isEmpty()) {
            err() << "no model: pass -m <model> (or --compare a,b)\n";
            err().flush();
            return 2;
        }
        models << m;
    }

    const QVector<EvalTask> tasks = Evals::suite(o.only);
    if (tasks.isEmpty()) {
        err() << (o.only.isEmpty() ? QStringLiteral("the suite is empty\n")
                                   : QStringLiteral("no eval task named '%1'\n").arg(o.only));
        err().flush();
        return 2;
    }

    if (!o.json) {
        out() << "eval · " << tasks.size() << " task" << (tasks.size() == 1 ? "" : "s") << " · "
              << models.join(", ") << "\n\n";
        out().flush();
    }

    // Live ticks as tasks land. They finish out of order — that is the point of
    // running them concurrently — so each line names its own task and model.
    const auto tick = [&](const EvalResult& r) {
        if (o.json) return;
        const QString mark = r.skipped ? QStringLiteral("–") : (r.pass ? QStringLiteral("✓")
                                                                       : QStringLiteral("✗"));
        out() << QStringLiteral("  %1 %2 %3 %4s  %5\n")
                     .arg(mark)
                     .arg(models.size() > 1 ? r.model.left(22) : QString(), models.size() > 1 ? -23 : 0)
                     .arg(r.name, -18)
                     .arg(r.ms / 1000.0, 5, 'f', 1)
                     .arg(r.pass ? QString() : r.detail);
        out().flush();
    };

    const QVector<EvalResult> results = Evals::run(tasks, o.backend, models, o.keep, tick);

    // Pass rate per model. Skipped tasks are out of the denominator: a check whose
    // interpreter is not installed says nothing about the model.
    QMap<QString, int> pass, ran, skipped;
    for (const EvalResult& r : results) {
        if (r.skipped) {
            ++skipped[r.model];
            continue;
        }
        ++ran[r.model];
        if (r.pass) ++pass[r.model];
    }

    double worst = 100.0;
    for (const QString& m : models) {
        const int n = ran.value(m);
        const double rate = n > 0 ? 100.0 * pass.value(m) / n : 0.0;
        worst = qMin(worst, rate);
    }

    if (o.json) {
        QJsonArray items;
        for (const EvalResult& r : results) {
            items.append(QJsonObject{{"model", r.model},
                                     {"task", r.name},
                                     {"pass", r.pass},
                                     {"skipped", r.skipped},
                                     {"ms", r.ms},
                                     {"detail", r.detail}});
        }
        QJsonObject rates;
        for (const QString& m : models) {
            const int n = ran.value(m);
            rates.insert(m, QJsonObject{{"passed", pass.value(m)},
                                        {"ran", n},
                                        {"skipped", skipped.value(m)},
                                        {"rate", n > 0 ? 100.0 * pass.value(m) / n : 0.0}});
        }
        out() << QString::fromUtf8(json::encode(QJsonObject{
                     {"tasks", tasks.size()}, {"models", rates}, {"results", items}}))
              << "\n";
        out().flush();
    } else {
        out() << "\n";
        for (const QString& m : models) {
            const int n = ran.value(m);
            const double rate = n > 0 ? 100.0 * pass.value(m) / n : 0.0;
            out() << QStringLiteral("  %1 %2/%3  %4%")
                         .arg(m, -24)
                         .arg(pass.value(m))
                         .arg(n)
                         .arg(rate, 5, 'f', 1);
            if (skipped.value(m) > 0) out() << "  (" << skipped.value(m) << " skipped)";
            out() << "\n";
        }
        out().flush();
    }

    // The floor is what makes this usable in CI: "this model is good enough to ship".
    if (o.min >= 0 && worst < o.min) {
        err() << QStringLiteral("✗ pass rate %1% is below --min %2%\n")
                     .arg(worst, 0, 'f', 1)
                     .arg(o.min, 0, 'f', 1);
        err().flush();
        return 1;
    }
    return 0;
}

// ------------------------------------------------------------------- config

// `true`/`false`/`null` and numbers are stored typed so `config get` round-trips
// them (0.5 stays a JSON number, not the string "0.5"); everything else is a
// string. Mirrors the PHP `config set` coercion.
QJsonValue parseConfigValue(const QString& raw) {
    if (raw == "true") return true;
    if (raw == "false") return false;
    if (raw == "null") return QJsonValue(QJsonValue::Null);
    bool isInt = false;
    const qlonglong iv = raw.toLongLong(&isInt);
    if (isInt) return QJsonValue(double(iv));
    bool isDouble = false;
    const double dv = raw.toDouble(&isDouble);
    if (isDouble) return QJsonValue(dv);
    return raw;
}

QString renderConfigValue(const QJsonValue& v) {
    switch (v.type()) {
        case QJsonValue::Null: return QStringLiteral("null");
        case QJsonValue::Bool: return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        case QJsonValue::Double: return QString::number(v.toDouble(), 'g', 15);
        case QJsonValue::String: return v.toString();
        case QJsonValue::Array: return QString::fromUtf8(json::encode(v.toArray()));
        case QJsonValue::Object: return QString::fromUtf8(json::encode(v.toObject()));
        default: return {};
    }
}

int cmdConfig(const QStringList& args) {
    const QString sub = args.value(0, QStringLiteral("get"));

    if (sub == "set") {
        const QString key = args.value(1);
        if (key.isEmpty()) {
            err() << "usage: ollamadev config set <key> <value>\n";
            err().flush();
            return 2;
        }
        // setPref writes ade-prefs.json, never config.json (MCP-only by convention).
        const QJsonValue val = parseConfigValue(args.value(2));
        Config::setPref(key, val);
        out() << "set " << key << " = " << renderConfigValue(val) << "\n";
        out().flush();
        return 0;
    }

    if (sub == "get") {
        const QString key = args.value(1);
        if (key.isEmpty()) {
            err() << "usage: ollamadev config get <key>\n";
            err().flush();
            return 2;
        }
        const QJsonValue v = Config::get(key);
        if (v.isUndefined()) {
            err() << "no such key: " << key << "\n";
            err().flush();
            return 1;
        }
        out() << renderConfigValue(v) << "\n";
        out().flush();
        return 0;
    }

    err() << "usage: ollamadev config get <key> | config set <key> <value>\n";
    err().flush();
    return 2;
}

// ------------------------------------------------------------------- models

QStringList installedOllamaModels() {
    auto b = Backends::get("ollama");
    return (b && b->available()) ? b->models() : QStringList{};
}

QJsonObject presetJson(const Preset& p, bool cloud, bool installed) {
    return QJsonObject{{"alias", p.alias},   {"tag", p.tag},       {"size", p.size},
                       {"tools", p.tools},   {"vision", p.vision}, {"role", p.role},
                       {"note", p.note},     {"cloud", cloud},     {"installed", installed}};
}

void printPresetRow(const Preset& p, const QStringList& installed) {
    const bool have = !Models::match(p.tag, installed).isEmpty();
    out() << QStringLiteral("  %1 %2 %3 %4 %5\n      %6\n")
                 .arg(p.alias, -18)
                 .arg(p.tag, -24)
                 .arg(p.size.isEmpty() ? QStringLiteral("cloud") : p.size, -8)
                 .arg(p.tools ? QStringLiteral("tools") : QStringLiteral("no-tools"), -8)
                 .arg(have ? QStringLiteral("✓ installed") : QStringLiteral("not installed"))
                 .arg(p.note);
}

int cmdModelsPresets(bool jsonOut) {
    const QStringList installed = installedOllamaModels();
    if (jsonOut) {
        QJsonArray a;
        for (const Preset& p : Models::presets())
            a.append(presetJson(p, false, !Models::match(p.tag, installed).isEmpty()));
        for (const Preset& p : Models::cloudPresets())
            a.append(presetJson(p, true, !Models::match(p.tag, installed).isEmpty()));
        out() << QString::fromUtf8(json::encode(a)) << "\n";
        out().flush();
        return 0;
    }
    out() << "Recommended models (pull with: ollamadev pull <alias>)\n\n";
    out() << "  Local — run on your machine\n";
    for (const Preset& p : Models::presets()) printPresetRow(p, installed);
    out() << "\n  Cloud — run on Ollama's servers (needs `ollama signin`); prompts leave "
             "this machine\n";
    for (const Preset& p : Models::cloudPresets()) printPresetRow(p, installed);
    out().flush();
    return 0;
}

int cmdModelsCloud(bool jsonOut) {
    const QStringList installed = installedOllamaModels();
    if (jsonOut) {
        QJsonArray a;
        for (const Preset& p : Models::cloudPresets())
            a.append(presetJson(p, true, !Models::match(p.tag, installed).isEmpty()));
        out() << QString::fromUtf8(json::encode(a)) << "\n";
        out().flush();
        return 0;
    }
    out() << "Ollama cloud models\n";
    out() << "Run on Ollama's servers, reached through your local daemon — still Ollama-only,\n"
             "but prompts leave this machine. Frontier-scale models without the local VRAM.\n\n";
    out() << "  1. sign in once:  ollama signin\n"
             "  2. pull one:      ollamadev pull <alias|tag>\n"
             "  3. use it:        ollamadev -m <tag>\n\n";
    for (const Preset& p : Models::cloudPresets()) printPresetRow(p, installed);
    out() << "\nAny `<name>-cloud` / `:cloud` tag works too, not just these.\n";
    out().flush();
    return 0;
}

int cmdModelsChain(bool jsonOut) {
    const QStringList installed = installedOllamaModels();
    const QStringList chain = Models::chain();
    const QString best = Models::bestInstalled(installed);
    const bool configured = !Config::get("model.fallback").isUndefined();
    const bool autoFallback = Config::boolean("model.autoFallback", true);

    if (jsonOut) {
        QJsonArray rows;
        for (const QString& t : chain)
            rows.append(QJsonObject{{"tag", t}, {"installed", !Models::match(t, installed).isEmpty()}});
        out() << QString::fromUtf8(json::encode(QJsonObject{
                     {"chain", rows}, {"best", best}, {"autoFallback", autoFallback}}))
              << "\n";
        out().flush();
        return 0;
    }
    out() << "Tool-calling fallback chain"
          << (configured ? " (from config model.fallback)" : " (default)") << ":\n";
    for (const QString& t : chain) {
        const QString hit = Models::match(t, installed);
        out() << "  " << (hit.isEmpty() ? "·" : "✓") << " " << t
              << (!hit.isEmpty() && hit != t ? QStringLiteral(" (%1)").arg(hit) : QString()) << "\n";
    }
    out() << "\n  Best installed: "
          << (best.isEmpty() ? QStringLiteral("none — pull one: ollamadev pull qwen2.5-coder")
                             : best)
          << "\n";
    out() << "  Auto-fallback when a model lacks tool support: " << (autoFallback ? "on" : "off")
          << "  (config model.autoFallback)\n";
    out().flush();
    return 0;
}

// ------------------------------------------------------- sessions (chat/load/resume)

QString relativeTime(qint64 unixSecs) {
    if (unixSecs <= 0) return QStringLiteral("unknown");
    const qint64 diff = QDateTime::currentSecsSinceEpoch() - unixSecs;
    if (diff < 60) return QStringLiteral("just now");
    if (diff < 3600) return QStringLiteral("%1m ago").arg(diff / 60);
    if (diff < 86400) return QStringLiteral("%1h ago").arg(diff / 3600);
    return QStringLiteral("%1d ago").arg(diff / 86400);
}

void printSessionRow(int n, const SessionMeta& s) {
    QString title = s.title.simplified();
    if (title.isEmpty()) title = QStringLiteral("(untitled)");
    if (title.size() > 50) title = title.left(50) + QStringLiteral("…");
    out() << QStringLiteral("  %1  %2 %3 · %4 msg · %5\n")
                 .arg(n, 2)
                 .arg(title, -52)
                 .arg(s.model)
                 .arg(s.messages)
                 .arg(relativeTime(s.updated));
}

// Shared by `resume`, `chat` (no prompt when stdin is not a tty), etc.: run the
// REPL on a specific session, in chat mode or agent mode as asked.
int runRepl(const QString& sessionId, bool chat, const QStringList& args) {
    ReplOptions o;
    o.backend = flagValue(args, "--backend");
    o.model = flagValue(args, "--model", flagValue(args, "-m"));
    o.sessionId = sessionId;
    o.chat = chat;
    return Repl(o).run();
}

int cmdLoad(const QStringList& args) {
    const QString id = args.value(0);
    if (id.isEmpty() || id.startsWith('-')) {
        err() << "usage: ollamadev load <session-id>   (list them: ollamadev resume)\n";
        err().flush();
        return 2;
    }
    if (!Session::load(id)) {
        err() << "session not found: " << id << "\n";
        err().flush();
        return 1;
    }
    return runRepl(id, false, args);
}

int cmdResume(const QStringList& args) {
    QVector<SessionMeta> sessions = Session::list();  // newest first
    if (sessions.isEmpty()) {
        out() << "  No previous sessions to resume.\n";
        out().flush();
        return 0;
    }
    if (sessions.size() > 20) sessions.resize(20);

    out() << "\n  Resume a session\n\n";
    for (int i = 0; i < sessions.size(); ++i) printSessionRow(i + 1, sessions.at(i));
    out() << "\n  Enter a number to resume (or blank to cancel): ";
    out().flush();

    const QString choice = QTextStream(stdin).readLine().trimmed();
    bool ok = false;
    const int idx = choice.toInt(&ok) - 1;
    if (choice.isEmpty() || !ok) {
        out() << "  cancelled.\n";
        out().flush();
        return 0;
    }
    if (idx < 0 || idx >= sessions.size()) {
        out() << "  out of range.\n";
        out().flush();
        return 1;
    }
    return runRepl(sessions.at(idx).id, false, args);
}

// Tool-free chat threads. `chat` (with an optional prompt) is a REPL in chat mode;
// list/delete manage the same per-project session store the agent uses.
int cmdChat(const QStringList& args) {
    const QString sub = args.value(0);

    if (sub == "list") {
        const auto sessions = Session::list();
        if (sessions.isEmpty()) {
            out() << "no chats yet — start one: ollamadev chat\n";
            out().flush();
            return 0;
        }
        for (int i = 0; i < sessions.size(); ++i) {
            const SessionMeta& s = sessions.at(i);
            out() << "  " << s.id << "  " << (s.title.isEmpty() ? QStringLiteral("(untitled)")
                                                                : s.title.simplified())
                  << "  " << s.model << " · " << s.messages << " msg · "
                  << relativeTime(s.updated) << "\n";
        }
        out().flush();
        return 0;
    }

    if (sub == "delete" || sub == "rm") {
        const QString id = args.value(1);
        if (id.isEmpty()) {
            err() << "usage: ollamadev chat delete <session-id>\n";
            err().flush();
            return 2;
        }
        if (!Session::remove(id)) {
            err() << "no chat '" << id << "'\n";
            err().flush();
            return 1;
        }
        out() << "✓ removed " << id << "\n";
        out().flush();
        return 0;
    }

    // Anything else is the opening prompt (join the non-flag words), or empty for a
    // bare interactive chat.
    static const QStringList takesValue{"--backend", "--model", "-m"};
    QStringList words;
    for (int i = 0; i < args.size(); ++i) {
        if (args.at(i).startsWith('-')) {
            if (takesValue.contains(args.at(i))) ++i;
            continue;
        }
        words << args.at(i);
    }
    ReplOptions o;
    o.backend = flagValue(args, "--backend");
    o.model = flagValue(args, "--model", flagValue(args, "-m"));
    o.chat = true;
    o.initialPrompt = words.join(' ');
    return Repl(o).run();
}

// --------------------------------------------------------------- completion

int cmdCompletion(const QStringList& args) {
    const QString shell = args.value(0, QStringLiteral("bash"));

    // The top-level verbs the CLI dispatches, kept here so the completion offers the
    // same set the code actually handles.
    static const char* kTopLevel =
        "chat models backends doctor crew board index code-search search skills memory "
        "config completion load resume pull setup context stats diff commit ship pr git "
        "test verify watch terminal hooks commands mcp scan voice transcribe lsp eval "
        "help --help --version --backend --model --continue --resume --new -h -v -m -c";

    if (shell == "bash") {
        out() << "# OllamaDev bash completion — install: ollamadev completion bash >> ~/.bashrc\n"
                 "_ollamadev() {\n"
                 "    local cur prev\n"
                 "    COMPREPLY=()\n"
                 "    cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
                 "    prev=\"${COMP_WORDS[COMP_CWORD-1]}\"\n"
                 "    case \"${prev}\" in\n"
                 "        models)\n"
                 "            COMPREPLY=($(compgen -W 'presets cloud chain' -- \"${cur}\"))\n"
                 "            return 0 ;;\n"
                 "        config)\n"
                 "            COMPREPLY=($(compgen -W 'get set' -- \"${cur}\"))\n"
                 "            return 0 ;;\n"
                 "        completion)\n"
                 "            COMPREPLY=($(compgen -W 'bash zsh fish' -- \"${cur}\"))\n"
                 "            return 0 ;;\n"
                 "        chat)\n"
                 "            COMPREPLY=($(compgen -W 'list delete' -- \"${cur}\"))\n"
                 "            return 0 ;;\n"
                 "        crew)\n"
                 "            COMPREPLY=($(compgen -W 'accept discard steer role pack clear' -- \"${cur}\"))\n"
                 "            return 0 ;;\n"
                 "        terminal)\n"
                 "            COMPREPLY=($(compgen -W 'create spawn list attach start stop broadcast send delete log' -- \"${cur}\"))\n"
                 "            return 0 ;;\n"
                 "        git)\n"
                 "            COMPREPLY=($(compgen -W 'status diff log branch checkout commit add push pull stash show' -- \"${cur}\"))\n"
                 "            return 0 ;;\n"
                 "        --backend)\n"
                 "            COMPREPLY=($(compgen -W 'ollama claude codex gemini cursor-agent opencode qwen aider goose amp crush droid' -- \"${cur}\"))\n"
                 "            return 0 ;;\n"
                 "        *)\n"
                 "            COMPREPLY=($(compgen -W '"
              << kTopLevel
              << "' -- \"${cur}\")) ;;\n"
                 "    esac\n"
                 "    return 0\n"
                 "}\n"
                 "complete -F _ollamadev ollamadev\n";
        out().flush();
        return 0;
    }

    if (shell == "zsh") {
        out() << "#compdef ollamadev\n"
                 "_ollamadev() {\n"
                 "    local -a commands\n"
                 "    commands=(\n"
                 "        'chat:Tool-free chat thread'\n"
                 "        'models:List models (presets, cloud, chain)'\n"
                 "        'backends:Show installed providers'\n"
                 "        'crew:Run the parallel agent bench'\n"
                 "        'board:Pending decisions'\n"
                 "        'config:Get or set a config key'\n"
                 "        'load:Resume a specific session'\n"
                 "        'resume:Pick a recent session to resume'\n"
                 "        'pull:Download a model from Ollama'\n"
                 "        'git:Git porcelain'\n"
                 "        'lsp:Language server for IDEs'\n"
                 "        'eval:Measure the agent pass rate'\n"
                 "        'diff:Working-tree diff'\n"
                 "        'ship:Stage, scan, commit, push'\n"
                 "        'help:Show help'\n"
                 "    )\n"
                 "    _describe 'command' commands\n"
                 "}\n"
                 "_ollamadev \"$@\"\n";
        out().flush();
        return 0;
    }

    if (shell == "fish") {
        out() << "# OllamaDev fish completion\n"
                 "complete -c ollamadev -n '__fish_use_subcommand' -a 'chat models backends doctor "
                 "crew board config load resume pull git lsp eval diff commit ship pr' -d 'Command'\n"
                 "complete -c ollamadev -n '__fish_seen_subcommand_from models' -a 'presets cloud "
                 "chain' -d 'Models subcommand'\n"
                 "complete -c ollamadev -n '__fish_seen_subcommand_from config' -a 'get set' -d "
                 "'Config subcommand'\n"
                 "complete -c ollamadev -n '__fish_seen_subcommand_from git' -a 'status diff log "
                 "branch checkout commit add push pull stash show' -d 'Git command'\n"
                 "complete -c ollamadev -s h -l help -d 'Show help'\n"
                 "complete -c ollamadev -s v -l version -d 'Show version'\n"
                 "complete -c ollamadev -s m -l model -d 'Use a specific model' -r\n";
        out().flush();
        return 0;
    }

    err() << "usage: ollamadev completion [bash|zsh|fish]\n";
    err().flush();
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    Config::load();
    Tools::registerAll();

    QStringList args = QCoreApplication::arguments();
    args.removeFirst();

    if (hasFlag(args, "-h") || hasFlag(args, "--help")) {
        printHelp();
        return 0;
    }
    if (hasFlag(args, "-v") || hasFlag(args, "--version")) {
        out() << "OllamaDev " << ODV_VERSION << "\n";
        out().flush();
        return 0;
    }

    // Take the agent off the network for THIS run, whatever config says. Checked
    // when a network tool runs, so it applies to the REPL and the crew too.
    if (hasFlag(args, "--no-web")) WebSearch::setWebEnabled(false);

    // Nothing to do but talk => the interactive REPL. "No args" also covers a bare
    // `ollamadev -m <model>` / `-c`: those select a model and a session, they are
    // not a one-shot prompt, and dropping into help there is just a dead end.
    if (positionals(args).isEmpty()) {
        // Reconcile with the shared current project before anything reads the cwd:
        // this may chdir into the active workspace, and session resume / tools all
        // resolve against QDir::currentPath().
        syncCurrentProject();
        ReplOptions o;
        o.backend = flagValue(args, "--backend");
        o.model = flagValue(args, "--model", flagValue(args, "-m"));
        // Auto-resume this folder's most recent session by default; --new forces a
        // fresh one. -c/--continue stay as (now redundant) explicit opt-ins. The
        // first run in a folder has nothing to resume and starts fresh silently.
        o.resume = !hasFlag(args, "--new");
        return Repl(o).run();
    }

    const QString cmd = args.first();
    const QStringList rest = args.mid(1);

    // Must be early and must not fall through anything that prints: on the stdio
    // path `lsp` owns stdout the moment it starts.
    if (cmd == "lsp") return cmdLsp(rest);
    if (cmd == "eval") return cmdEval(rest);

    if (cmd == "mcp") return cmdMcp(rest);
    if (cmd == "skills") return cmdSkills(rest);
    if (cmd == "memory") return cmdMemory(rest);
    if (cmd == "index") return cmdIndex(rest);
    if (cmd == "code-search") return cmdCodeSearch(rest);
    if (cmd == "search") return cmdSearch(rest);

    if (cmd == "backends") return cmdBackends();
    if (cmd == "agents") return cmdAgents(rest);
    if (cmd == "doctor") return cmdDoctor();

    if (cmd == "pull") {
        const QString model = positionals(args).value(1);
        if (model.isEmpty()) {
            err() << "usage: ollamadev pull <model>\n";
            err().flush();
            return 2;
        }
        QString e;
        const bool ok = Puller::pull(model, {}, &e);
        if (!ok && !e.isEmpty()) {
            err() << "✗ " << e << "\n";
            err().flush();
        }
        return ok ? 0 : 1;
    }

    if (cmd == "setup") {
        // The onboarding asks before pulling a model, so it needs a real y/N.
        return Onboard::run([](const QString& q) -> bool {
            out() << q << " [Y/n] ";
            out().flush();
            QString line = QTextStream(stdin).readLine().trimmed().toLower();
            return line.isEmpty() || line == "y" || line == "yes";
        });
    }

    if (cmd == "context") {
        out() << ContextTuner::report();
        out().flush();
        return 0;
    }

    if (cmd == "route") {
        // `route "<prompt>"` shows which model the brain would pick (and why);
        // `route --run "<prompt>"` then answers on it.
        const QString prompt = positionals(args).mid(1).join(' ');
        if (prompt.isEmpty()) {
            err() << "usage: ollamadev route [--run] \"<prompt>\"\n";
            err().flush();
            return 2;
        }
        const RouteDecision d = Router::pick(prompt);
        out() << "→ " << d.tier << "  " << d.backend << ":" << d.model << "  (" << d.reason
              << ")\n";
        out().flush();
        if (!hasFlag(args, "--run")) return 0;
        // Answer on the chosen model.
        Agent a(d.backend, d.model);
        Permission::setMode(PermMode::Auto);
        Tools::setThreadRoot(QDir::currentPath());
        QVector<ChatMessage> msgs{
            {"system", a.buildSystemPrompt(QDir::currentPath()), {}, {}, {}, {}, {}},
            {"user", prompt, {}, {}, {}, {}, {}}};
        StreamSink sink;
        sink.onContent = [](const QString& c) {
            out() << c;
            out().flush();
        };
        CancelToken cancel;
        a.loop(msgs, Config::integer("agents.maxIterations", 20), sink, cancel);
        out() << "\n";
        out().flush();
        return 0;
    }

    if (cmd == "stats") {
        out() << Usage::report();
        out().flush();
        return 0;
    }
    if (cmd == "board") return cmdBoard(rest);
    if (cmd == "ws" || cmd == "workspace") return cmdWorkspace(rest);
    if (cmd == "plugin" || cmd == "plugins") return cmdPlugin(rest);
    if (cmd == "tidy") return cmdTidy(rest);
    if (cmd == "acp") return Acp::serve();
    if (cmd == "update" || cmd == "upgrade") return cmdUpdate(rest);
    if (cmd == "export") return cmdExport(rest);
    if (cmd == "import") return cmdImport(rest);
    if (cmd == "scan") return cmdScan(rest);
    if (cmd == "voice") return cmdVoice(rest);
    if (cmd == "transcribe") return cmdTranscribe(rest);

    if (cmd == "terminal") return cmdTerminal(rest);
    if (cmd == "watch") return cmdWatch(rest);
    if (cmd == "hooks") return cmdHooks(rest);
    if (cmd == "commands") return cmdCommands(rest);

    // A user-defined slash command typed at the shell: `ollamadev /review src/x.c`
    // expands ~/.ollamadev/commands/review.md and runs it as a one-shot.
    if (cmd.startsWith('/') && UserCmds::exists(cmd.mid(1))) {
        const QString prompt = UserCmds::expand(cmd.mid(1), rest.join(' '));
        if (!prompt.isEmpty()) return cmdOneShot(prompt, args);
    }

    if (cmd == "diff") return cmdDiff(rest);
    if (cmd == "commit") return cmdCommit(rest);
    if (cmd == "ship") return cmdShip(rest);
    if (cmd == "pr") return cmdPr(rest);
    if (cmd == "git") return cmdGit(rest);
    if (cmd == "test") return cmdTest(rest);
    if (cmd == "verify") return cmdVerify(rest);

    if (cmd == "config") return cmdConfig(rest);
    if (cmd == "completion") return cmdCompletion(rest);
    if (cmd == "chat") return cmdChat(rest);
    if (cmd == "load") return cmdLoad(rest);
    if (cmd == "resume") return cmdResume(rest);

    if (cmd == "models") {
        // presets / cloud / chain are catalog views; a bare `models` still lists what
        // is installed on the active backend.
        const QString sub = rest.value(0);
        const bool jsonOut = hasFlag(args, "--json");
        if (sub == "presets" || sub == "recommended") return cmdModelsPresets(jsonOut);
        if (sub == "cloud") return cmdModelsCloud(jsonOut);
        if (sub == "chain") return cmdModelsChain(jsonOut);

        const QString id = flagValue(args, "--backend", Config::str("model.backend", "ollama"));
        auto b = Backends::get(id);
        if (!b || !b->available()) {
            err() << id << " is not available\n";
            err().flush();
            return 1;
        }
        for (const auto& m : b->models()) out() << "  " << m << "\n";
        out().flush();
        return 0;
    }

    if (cmd == "crew") {
        const QString sub = rest.value(0);
        QString e;
        if (sub == "role") return cmdCrewRole(rest.mid(1));
        if (sub == "pack") return cmdCrewPack(rest.mid(1));
        if (sub == "resume") return cmdCrewResume(rest.mid(1));
        if (sub == "accept") {
            if (Crew::accept(rest.value(1).toInt(), &e)) {
                out() << "✓ applied\n";
                out().flush();
                return 0;
            }
            err() << "✗ " << e << "\n";
            err().flush();
            return 1;
        }
        if (sub == "discard") {
            if (Crew::discard(rest.value(1).toInt(), &e)) {
                out() << "✓ discarded\n";
                out().flush();
                return 0;
            }
            err() << "✗ " << e << "\n";
            err().flush();
            return 1;
        }
        if (sub == "steer") {
            if (Crew::steer(rest.value(1).toInt(), rest.mid(2).join(' '), &e)) {
                out() << "✓ sent\n";
                out().flush();
                return 0;
            }
            err() << "✗ " << e << "\n";
            err().flush();
            return 1;
        }
        if (sub == "clear") {
            Crew::clearBoard();
            out() << "✓ board cleared\n";
            out().flush();
            return 0;
        }
        return cmdCrew(rest);
    }

    // Anything else is a prompt — the POSITIONAL words only. args.join(' ') would
    // splice the flags into the prompt itself, so `ollamadev -m qwen3.5:9b fix the
    // parser` asked the model to fix "-m qwen3.5:9b fix the parser".
    return cmdOneShot(positionals(args).join(' '), args);
}
