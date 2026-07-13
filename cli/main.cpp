#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>

#include "Agent.h"
#include "Backend.h"
#include "Board.h"
#include "Config.h"
#include "Crew.h"
#include "Json.h"
#include "Models.h"
#include "Parallel.h"
#include "SecScan.h"
#include "Tools.h"
#include "Version.h"

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

QStringList flagList(const QStringList& a, const QString& f) {
    const QString v = flagValue(a, f);
    if (v.isEmpty()) return {};
    return v.split(',', Qt::SkipEmptyParts);
}

void printHelp() {
    out() << "OllamaDev " << ODV_VERSION << " — Ollama and every major coding CLI, in parallel\n\n"
          << "Usage: ollamadev [command] [options]\n\n"
          << "  ollamadev \"<prompt>\"        one-shot agent turn\n"
          << "  ollamadev backends           which providers are installed and how wide they run\n"
          << "  ollamadev models             list models on the active backend\n"
          << "  ollamadev crew \"<task>\"      the parallel bench (research → plan → N coders → audit)\n"
          << "  ollamadev crew accept <n>    apply held work into your folder\n"
          << "  ollamadev crew discard <n>   throw held work away\n"
          << "  ollamadev crew steer <n> \"…\" talk to a running coder\n"
          << "  ollamadev board              pending decisions\n"
          << "  ollamadev scan [path]        secret scanner (exit 1 on a high finding)\n"
          << "  ollamadev doctor             health check\n\n"
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
          << "  --focus \"<text>\"\n";
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

int cmdCrew(const QStringList& args) {
    CrewOptions o;
    o.task = args.value(0);
    if (o.task.startsWith('-')) o.task.clear();

    o.maxCoders = flagValue(args, "--max", "4").toInt();
    o.parallel = flagValue(args, "--parallel", "0").toInt();
    o.focus = flagValue(args, "--focus");
    o.research = !hasFlag(args, "--no-research");
    o.audit = !hasFlag(args, "--no-audit");
    o.land = hasFlag(args, "--review") ? "review" : Config::str("crew.land", "auto");
    o.coderBackend = flagValue(args, "--coder-backend");
    o.coderModel = flagValue(args, "--coder-model");
    o.coderBackends = flagList(args, "--coder-backends");
    o.coderModels = flagList(args, "--coder-models");
    o.directorBackend = flagValue(args, "--director-backend");
    o.directorModel = flagValue(args, "--director-model");
    o.auditorBackend = flagValue(args, "--auditor-backend");
    o.auditorModel = flagValue(args, "--auditor-model");
    o.researcherBackend = flagValue(args, "--researcher-backend");
    o.researcherModel = flagValue(args, "--researcher-model");

    if (o.task.isEmpty()) {
        err() << "crew needs a task: ollamadev crew \"build X\"\n";
        err().flush();
        return 2;
    }

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
    const QString path = args.value(0, QDir::currentPath());
    const auto findings = SecScan::scanFile(path);
    int high = 0;
    for (const auto& f : findings) {
        if (f.severity == "high") ++high;
        out() << "  " << f.severity << "  " << f.rule << "  line " << f.line << "  "
              << f.redacted << "\n";
    }
    if (findings.isEmpty()) out() << "clean\n";
    out().flush();
    return high > 0 ? 1 : 0;
}

int cmdDoctor() {
    Config::load();
    out() << "OllamaDev " << ODV_VERSION << "\n";
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

int cmdOneShot(const QString& prompt, const QStringList& args) {
    const QString backend =
        flagValue(args, "--backend", Config::str("model.backend", "ollama"));
    QString model = flagValue(args, "--model", flagValue(args, "-m"));
    if (model.isEmpty()) {
        auto b = Backends::get(backend);
        model = b ? b->defaultModel() : QString();
    }

    Agent a(backend, model);
    Permission::setMode(PermMode::Auto);
    Permission::setInteractive(true);
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

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    Config::load();
    Tools::registerAll();

    QStringList args = QCoreApplication::arguments();
    args.removeFirst();

    if (args.isEmpty() || hasFlag(args, "-h") || hasFlag(args, "--help")) {
        printHelp();
        return 0;
    }
    if (hasFlag(args, "-v") || hasFlag(args, "--version")) {
        out() << "OllamaDev " << ODV_VERSION << "\n";
        out().flush();
        return 0;
    }

    const QString cmd = args.first();
    const QStringList rest = args.mid(1);

    if (cmd == "backends") return cmdBackends();
    if (cmd == "doctor") return cmdDoctor();
    if (cmd == "board") return cmdBoard(rest);
    if (cmd == "scan") return cmdScan(rest);

    if (cmd == "models") {
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

    // Anything else is a prompt.
    return cmdOneShot(args.join(' '), args);
}
