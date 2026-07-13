#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>

#include "Agent.h"
#include "Backend.h"
#include "Board.h"
#include "CodeIndex.h"
#include "Config.h"
#include "Crew.h"
#include "Json.h"
#include "Mcp.h"
#include "Memory.h"
#include "Models.h"
#include "Parallel.h"
#include "Repl.h"
#include "SecScan.h"
#include "Skills.h"
#include "Stt.h"
#include "Tools.h"
#include "Version.h"
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
        "--session"};
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

void printHelp() {
    out() << "OllamaDev " << ODV_VERSION << " — Ollama and every major coding CLI, in parallel\n\n"
          << "Usage: ollamadev [command] [options]\n\n"
          << "  ollamadev                    interactive chat (-c resumes this folder's session)\n"
          << "  ollamadev \"<prompt>\"        one-shot agent turn\n"
          << "  ollamadev backends           which providers are installed and how wide they run\n"
          << "  ollamadev models             list models on the active backend\n"
          << "  ollamadev doctor             health check\n\n"
          << "Crew — the parallel bench (research → plan → N coders → audit → land):\n"
          << "  ollamadev crew \"<task>\"\n"
          << "  ollamadev crew accept <n>    apply held work into your folder\n"
          << "  ollamadev crew discard <n>   throw held work away\n"
          << "  ollamadev crew steer <n> \"…\" talk to a running coder\n"
          << "  ollamadev crew role|pack     personas the Director assigns · saved crew configs\n"
          << "  ollamadev board              pending decisions\n\n"
          << "Context:\n"
          << "  ollamadev index build        semantic code index (also: status, clear)\n"
          << "  ollamadev code-search \"<q>\"  search the repo by meaning\n"
          << "  ollamadev search \"<q>\"       web search\n"
          << "  ollamadev skills             progressive-disclosure skills (list/add/install)\n"
          << "  ollamadev memory             wiki-linked notes (new/list/show/graph)\n\n"
          << "Integration:\n"
          << "  ollamadev mcp serve          expose these tools to any MCP client (stdio)\n"
          << "  ollamadev mcp list|add|rm    MCP servers this agent can call\n"
          << "  ollamadev scan [path]        secret scanner (exit 1 on a high finding)\n"
          << "  ollamadev voice              record the mic and transcribe it (100% local)\n"
          << "                               --setup fetch the engine · --model <size> ·\n"
          << "                               --history [n] · --clear\n"
          << "  ollamadev transcribe <file>  transcribe an audio file\n\n"
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

    out() << "🎤 recording (" << Stt::modelSize() << ") — press Enter to stop…";
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
        ReplOptions o;
        o.backend = flagValue(args, "--backend");
        o.model = flagValue(args, "--model", flagValue(args, "-m"));
        o.resume = hasFlag(args, "-c") || hasFlag(args, "--continue");
        return Repl(o).run();
    }

    const QString cmd = args.first();
    const QStringList rest = args.mid(1);

    if (cmd == "mcp") return cmdMcp(rest);
    if (cmd == "skills") return cmdSkills(rest);
    if (cmd == "memory") return cmdMemory(rest);
    if (cmd == "index") return cmdIndex(rest);
    if (cmd == "code-search") return cmdCodeSearch(rest);
    if (cmd == "search") return cmdSearch(rest);

    if (cmd == "backends") return cmdBackends();
    if (cmd == "doctor") return cmdDoctor();
    if (cmd == "board") return cmdBoard(rest);
    if (cmd == "scan") return cmdScan(rest);
    if (cmd == "voice") return cmdVoice(rest);
    if (cmd == "transcribe") return cmdTranscribe(rest);

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
        if (sub == "role") return cmdCrewRole(rest.mid(1));
        if (sub == "pack") return cmdCrewPack(rest.mid(1));
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
