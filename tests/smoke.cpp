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

    s << "\n" << passed << " passed, " << failed << " failed\n";
    s.flush();
    return failed == 0 ? 0 : 1;
}
