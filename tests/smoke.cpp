// Dependency-free smoke tests. Anything that needs a live Ollama or a real
// coding CLI is skipped rather than failed, so this stays runnable in CI.
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

#include "Backend.h"
#include "Config.h"
#include "Json.h"
#include "Models.h"
#include "Parallel.h"
#include "Sandbox.h"
#include "SecScan.h"

using namespace odv;

static int passed = 0;
static int failed = 0;

static void check(bool ok, const QString& what) {
    QTextStream s(stdout);
    if (ok) {
        ++passed;
        s << "  ok    " << what << "\n";
    } else {
        ++failed;
        s << "  FAIL  " << what << "\n";
    }
    s.flush();
}

static void testJson() {
    // Cloud models fence their JSON even when told not to.
    const auto fenced = json::objectFrom("```json\n{\"clean\":true}\n```");
    check(fenced.value("clean").toBool(), "decodeLoose strips a code fence");

    const auto prosey = json::objectFrom("Sure! Here you go:\n{\"a\":{\"b\":2}}\nHope that helps.");
    check(json::at(prosey, "a.b").toInt() == 2, "decodeLoose survives surrounding prose");

    const auto flat = json::expandDotted(QJsonObject{{"crew.coderModel", "x"}});
    check(flat.value("crew").toObject().value("coderModel").toString() == "x",
          "expandDotted nests flat keys");
}

static void testModels() {
    check(Models::isCloud("gpt-oss:20b-cloud"), "isCloud recognises a -cloud tag");
    check(Models::isCloud("deepseek-v4-pro:cloud"), "isCloud recognises a :cloud tag");
    check(!Models::isCloud("qwen3.5:9b"), "isCloud rejects a local tag");
    check(Models::firstCloud({"qwen3.5:9b", "gpt-oss:20b-cloud"}) == "gpt-oss:20b-cloud",
          "firstCloud picks the cloud tag");
    check(Models::paramSizeB("8x7b") > 50, "paramSizeB expands an MoE tag");
}

static void testSecScan() {
    const auto f = SecScan::scanDiff("+AWS_SECRET=AKIAIOSFODNN7EXAMPLE\n-nothing\n");
    check(!f.isEmpty(), "scanDiff flags a credential on an added line");
    bool leaked = false;
    for (const auto& x : f)
        if (x.redacted.contains("IOSFODNN7")) leaked = true;
    check(!leaked, "findings never echo the full secret");

    const auto clean = SecScan::scanDiff("-AWS_SECRET=AKIAIOSFODNN7EXAMPLE\n");
    check(clean.isEmpty(), "scanDiff ignores removed lines");
}

static void testSandbox() {
    QTemporaryDir proj, sand, store;
    QDir(proj.path()).mkpath("src");
    QFile a(proj.path() + "/src/keep.txt");
    a.open(QIODevice::WriteOnly);
    a.write("one\n");
    a.close();

    QString err;
    check(Sandbox::copyTree(proj.path(), sand.path() + "/c1", &err),
          "copyTree mirrors a project");

    // The coder creates a file in a folder that does not exist in the project.
    QDir(sand.path() + "/c1").mkpath("src/deep/nested");
    QFile b(sand.path() + "/c1/src/deep/nested/new.txt");
    b.open(QIODevice::WriteOnly);
    b.write("hello\n");
    b.close();

    const Changeset cs = Sandbox::capture(proj.path(), sand.path() + "/c1", store.path());
    check(!cs.empty(), "capture sees the new file");
    check(cs.created.contains("src/deep/nested/new.txt"), "capture records the nested path");

    QStringList wrote;
    check(Sandbox::apply(store.path(), proj.path(), &wrote, &err), "apply lands the changeset");
    check(QFile::exists(proj.path() + "/src/deep/nested/new.txt"),
          "accept creates missing folders and the file");

    // A changeset must never be able to write outside the project.
    check(!Sandbox::apply("/nonexistent", proj.path(), nullptr, &err),
          "apply refuses a missing store");
}

static void testLimiter() {
    // A local model and a cloud model must not share an admission slot: the
    // whole point is that a cloud coder is not throttled by the local GPU.
    Limiter::instance().setLimit("ollama:local", 2);
    Limiter::instance().setLimit("ollama:cloud", 8);
    check(Limiter::instance().limit("ollama:local") == 2, "local backend is capped tight");
    check(Limiter::instance().limit("ollama:cloud") == 8, "cloud backend fans out wide");

    std::atomic_int concurrent{0};
    std::atomic_int peak{0};
    const auto r = parallelRun(
        6, [](int) { return QStringLiteral("ollama:local"); },
        [&](int) -> QJsonObject {
            const int now = ++concurrent;
            int want = peak.load();
            while (now > want && !peak.compare_exchange_weak(want, now)) {
            }
            QThread::msleep(40);
            --concurrent;
            return {};
        });
    check(r.size() == 6, "parallelRun returns every result");
    check(peak.load() <= 2, "parallelRun never exceeds the backend's limit");
}

static void testBackends() {
    check(Backends::all().first() == "ollama", "ollama is the default backend");
    check(Backends::all().contains("claude") && Backends::all().contains("codex") &&
              Backends::all().contains("gemini"),
          "the major coding CLIs are registered");
    auto claude = Backends::get("claude");
    check(claude && !claude->supportsNativeTools(),
          "coding CLIs run their own agent loop, so we never feed them tool schemas");
    auto ollama = Backends::get("ollama");
    check(ollama && ollama->supportsNativeTools(), "ollama does native function calling");
    check(ollama && ollama->concurrencyLimit("gpt-oss:20b-cloud") >
                        ollama->concurrencyLimit("qwen3.5:9b"),
          "a cloud model is allowed more concurrency than a local one");
}

// ---------------------------------------------------------------------------
// Regressions carried over from the PHP app. Both of these shipped in v0.9.78
// and are the reason this port exists in its current shape. They are asserted
// against the SOURCE, because both are bugs of omission — you cannot catch
// "somebody added a global pkill" by exercising a happy path.
// ---------------------------------------------------------------------------

// Code only, comments stripped. A guard that scanned comments too would fire on
// the comment that documents the very flag it is guarding against — which is
// exactly what happened the first time.
static QString readSource(const QString& rel) {
    QFile f(QStringLiteral(ODV_SOURCE_DIR) + "/" + rel);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QString code;
    for (const QString& line : QString::fromUtf8(f.readAll()).split('\n')) {
        const int c = line.indexOf(QStringLiteral("//"));
        code += (c >= 0 ? line.left(c) : line) + '\n';
    }
    return code;
}

static void testNoGlobalProcessNuking() {
    // PHP bug: Desktop/ollamadev-ade/index.php:71 ran PtyManager::cleanupStale()
    // on boot — `pkill -9 -f '__pty-daemon__'` plus `rm -rf ~/.ollamadev/terminals/*`.
    // Launching a SECOND instance therefore killed the FIRST one's live terminals.
    // Here every Pty is a child QObject of the widget that spawned it, so there is
    // nothing global to clean. Keep it that way.
    bool clean = true;
    // core/Stt.cpp is in this list for the same reason: the PHP recorder
    // backgrounded `arecord` through a shell and stopped it with `kill -INT
    // <scraped pid>`. The port spawns it as a QProcess child and terminates that
    // handle, so a kill-by-name here would be the same class of bug returning.
    for (const QString& f : {QStringLiteral("ade/MainWindow.cpp"),
                             QStringLiteral("ade/TerminalWidget.cpp"),
                             QStringLiteral("ade/main.cpp"), QStringLiteral("core/Pty.cpp"),
                             QStringLiteral("core/Stt.cpp"),
                             // core/Terminals.cpp is THE file the bug lived in — it is the
                             // port of the multiplexer that ran the boot-time pkill. A guard
                             // that covered everything except the crime scene was theatre.
                             QStringLiteral("core/Terminals.cpp")}) {
        const QString src = readSource(f);
        if (src.isEmpty()) {
            clean = false;
            break;
        }
        // A process-wide kill by name, or wiping a shared terminals dir, would
        // reintroduce the second-instance bug.
        if (src.contains("pkill") || src.contains("killall") ||
            src.contains("cleanupStale")) {
            clean = false;
        }
    }
    check(clean, "no instance-global process kill (2nd ADE must not kill the 1st's terminals)");
}

static void testCliBackendIsSandboxed() {
    // A CLI backend is an agent in a subprocess: it does its OWN file edits, so
    // the thread-local tool root cannot confine it. Its working directory IS its
    // sandbox. Pointing it at the process cwd let a crew coder write straight
    // into the user's real project, bypassing the changeset, the auditor, the
    // secret gate and the overlap guard at once. Caught by a live mixed crew.
    const QString src = readSource(QStringLiteral("core/CliBackend.cpp"));
    check(src.contains("Tools::threadRoot()"),
          "CLI backends run in the coder's sandbox, not the process cwd");
    check(!src.contains("setWorkingDirectory(QDir::currentPath())"),
          "CLI backend never hardcodes the project root as its working directory");
}

static void testPerBackendModelRouting() {
    // A model tag belongs to exactly ONE backend. Two ways this broke a live
    // mixed crew, both caught by running one:
    //  1. inheriting the session's Ollama tag for a Claude/Codex coder — that
    //     CLI rejects the run with "model may not exist";
    //  2. `--coder-models tag,,` parsed with SkipEmptyParts collapsing to a
    //     one-element list, so the round-robin wrapped and handed coder 2 the
    //     Ollama tag anyway. These lists are POSITIONAL: entry i is coder i.
    const QString crew = readSource(QStringLiteral("core/Crew.cpp"));
    check(crew.contains("modelFor("),
          "each crew role resolves its model against its OWN backend");

    const QString cli = readSource(QStringLiteral("cli/main.cpp"));
    check(!cli.contains("v.split(',', Qt::SkipEmptyParts)"),
          "positional --coder-* lists keep empty slots (they mean 'backend default')");
}

static void testCliArgvIsCurrent() {
    // PHP bug: src/52-model-client.php:135 pinned flags that these CLIs removed,
    // so --backend claude|codex|qwen was broken in the shipping app. Assert we
    // never resurrect the dead flags.
    const QString src = readSource(QStringLiteral("core/CliBackend.cpp"));
    check(!src.isEmpty(), "CliBackend source is readable");
    check(!src.contains("--tools"), "claude: --tools was removed from the CLI");
    check(!src.contains("--ask-for-approval"),
          "codex: --ask-for-approval was removed from `exec`");
    check(!src.contains("--approval-mode plan") && !src.contains("--input-format"),
          "qwen: --approval-mode plan / --input-format do not exist");
    check(src.contains("--skip-trust"),
          "gemini: --skip-trust is required or yolo silently downgrades and hangs");
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    Config::load();

    QTextStream s(stdout);
    s << "json\n";       s.flush(); testJson();
    s << "models\n";     s.flush(); testModels();
    s << "secscan\n";    s.flush(); testSecScan();
    s << "sandbox\n";    s.flush(); testSandbox();
    s << "parallel\n";   s.flush(); testLimiter();
    s << "backends\n";   s.flush(); testBackends();
    s << "regressions\n"; s.flush(); testNoGlobalProcessNuking(); testCliArgvIsCurrent(); testCliBackendIsSandboxed(); testPerBackendModelRouting();

    s << "\n" << passed << " passed, " << failed << " failed\n";
    s.flush();
    return failed == 0 ? 0 : 1;
}
