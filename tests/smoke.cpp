// Dependency-free smoke tests. Anything that needs a live Ollama or a real
// coding CLI is skipped rather than failed, so this stays runnable in CI.
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

#include <QProcess>

#include "Backend.h"
#include "Config.h"
#include "Json.h"
#include "Models.h"
#include "Parallel.h"
#include "Sandbox.h"
#include "SecScan.h"
#include "Tools.h"
#include "Crew.h"
#include "Vision.h"
#include "Workspaces.h"

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

    // `crew resume` reloads a finished coder's changeset from disk instead of
    // re-running the model, so load() must round-trip capture().
    const Changeset reloaded = Sandbox::load(store.path());
    check(reloaded.created == cs.created && reloaded.diff == cs.diff,
          "Sandbox::load round-trips a captured changeset");
    check(Sandbox::load("/nonexistent/store").empty(), "Sandbox::load of a missing store is empty");

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

// `crew resume` must reload the saved plan and skip finished coders, never
// re-plan or re-run work that already landed.
static void testCrewResume() {
    const QString crew = readSource(QStringLiteral("core/Crew.cpp"));
    check(crew.contains("writePlan(") && crew.contains("plan.json"),
          "crew persists a durable plan.json so a run can be resumed");
    check(crew.contains("bool resuming") && crew.contains("opts.resumeRunId"),
          "run() branches on resumeRunId to reload the plan instead of re-planning");
    check(crew.contains("skipRun"),
          "resume skips coders that already finished (done/held)");
    check(crew.contains("replan") && crew.contains("doneTitles"),
          "resume re-plans only the leftover work, telling the Director what's done");
    check(crew.contains("reloaded") && crew.contains("audit.clean = true"),
          "finished coders land from disk and are never re-audited on resume");
    check(crew.contains("doRoute") && crew.contains("plan.value(\"route\")"),
          "brain settings (route/debate/dedupe) survive a resume via the plan");
    const QString cli = readSource(QStringLiteral("cli/main.cpp"));
    check(cli.contains("cmdCrewResume") && cli.contains("\"resume\""),
          "CLI wires `crew resume`");
    check(cli.contains("--replay"), "CLI exposes --replay (opt out of the default re-plan)");
}

// --amplify N: N Director plans (keep the modal one) + an N-reviewer audit panel
// that needs a STRICT majority. And a crew pack must actually reach a run — the
// pack keys were written to disk and then read by nobody, so `--amplify 3` in a
// saved pack silently did nothing.
static void testCrewAmplify() {
    const QString crew = readSource(QStringLiteral("core/Crew.cpp"));
    check(crew.contains("planOnce") && crew.contains("amplify <= 1"),
          "the Director draws one plan per sample and short-circuits at amplify=1");
    check(crew.contains("freq[c.size()]"),
          "self-consistency keeps the plan whose subtask COUNT is the mode");
    check(crew.contains("reviewOnce") && crew.contains("pass % 2 == 1"),
          "the audit panel alternates neutral and skeptic seats");
    check(crew.contains("clean * 2 > amplify"),
          "a changeset lands only on a STRICT majority of clean votes (2-2 holds)");
    check(crew.contains("plan.value(\"amplify\")"),
          "amplify rides in plan.json, so a resumed run re-plans with the same rigour");

    const QString cli = readSource(QStringLiteral("cli/main.cpp"));
    check(cli.contains("o.amplify = num(\"--amplify\", \"amplify\", 1)"),
          "CLI: --amplify wins over the pack, the pack over the default");
    check(cli.contains("CrewPacks::load(packName)"),
          "crew --pack loads a saved team as the base (it used to load nothing)");
    check(cli.contains("\"--amplify\",         \"--pack\""),
          "--amplify/--pack take a value, so they never swallow the task word");

    // The real bug this guards: a pack key that no run reads. If someone adds a
    // key to `crew pack save`, it has to be consumed in cmdCrew too.
    const int save = cli.indexOf(QStringLiteral("int cmdCrewPack"));
    const QString packSave = save < 0 ? QString() : cli.mid(save, 3000);
    for (const QString& key : {QStringLiteral("focus"), QStringLiteral("coderModel"),
                               QStringLiteral("amplify"), QStringLiteral("land"),
                               QStringLiteral("route")}) {
        check(packSave.contains(QStringLiteral("\"%1\"").arg(key)) &&
                  cli.contains(QStringLiteral("\"%1\"").arg(key)),
              QStringLiteral("pack key '%1' is both saved and read back").arg(key));
    }
}

// The agent tool suite, driven for real: a throwaway git repo, actual commits,
// an actual background job. These are the tools a model reaches for most, so they
// are tested by BEHAVIOUR, not by grepping the source.
static void testAgentTools() {
    Tools::registerAll();
    Permission::setMode(PermMode::Auto);
    Permission::setInteractive(false);

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        check(false, "temp dir for the tool tests");
        return;
    }
    const QString root = tmp.path();

    // A real repo, made with real git. If git is missing there is nothing to test.
    auto git = [&](const QStringList& a) {
        QProcess p;
        p.setWorkingDirectory(root);
        p.start(QStringLiteral("git"), a);
        return p.waitForFinished(10000) && p.exitCode() == 0;
    };
    if (!git({QStringLiteral("init"), QStringLiteral("-q")})) {
        check(true, "git is not installed — skipping the git tool tests");
        return;
    }
    git({QStringLiteral("config"), QStringLiteral("user.email"), QStringLiteral("t@t.t")});
    git({QStringLiteral("config"), QStringLiteral("user.name"), QStringLiteral("t")});

    // Every tool resolves against the thread root, so this IS the sandbox boundary.
    Tools::setThreadRoot(root);

    QFile f(root + QStringLiteral("/hello.txt"));
    f.open(QIODevice::WriteOnly);
    f.write("hi\n");
    f.close();

    ToolResult r = Tools::run(QStringLiteral("git_status"), {});
    check(r.ok && r.output.contains(QStringLiteral("hello.txt")), "git_status sees the new file");

    r = Tools::run(QStringLiteral("git_add"), QJsonObject{{"all", true}});
    check(r.ok, "git_add stages everything");

    // A multi-line message goes over stdin, so a newline in it can never be argv.
    r = Tools::run(QStringLiteral("git_commit"),
                   QJsonObject{{"message", QStringLiteral("first: add hello\n\nwith a body")}});
    check(r.ok, "git_commit commits the staged tree");

    r = Tools::run(QStringLiteral("git_log"), QJsonObject{{"n", 5}});
    check(r.ok && r.output.contains(QStringLiteral("first: add hello")), "git_log lists the commit");

    // The PHP original interpolated `n` into a shell string unescaped. If that ever
    // comes back, this injects a command that would create a file in the repo.
    Tools::run(QStringLiteral("git_log"),
               QJsonObject{{"n", QStringLiteral("5; touch pwned")}});
    check(!QFileInfo(root + QStringLiteral("/pwned")).exists(),
          "git_log cannot be used to inject a shell command");

    r = Tools::run(QStringLiteral("git_branch"), {});
    check(r.ok, "git_branch lists branches");

    r = Tools::run(QStringLiteral("git_show"), QJsonObject{{"stat", true}});
    check(r.ok && r.output.contains(QStringLiteral("hello.txt")), "git_show shows HEAD");

    // A crew coder lives under an explicit thread root. Pushing from there would
    // send un-landed sandbox work to the real remote, so it must refuse.
    r = Tools::run(QStringLiteral("git_push"), {});
    check(!r.ok && r.error.contains(QStringLiteral("sandbox")),
          "git_push REFUSES to push out of a sandbox");

    // A failing git command must come back as a FAILURE, not as success carrying
    // the error text — that was the bug in every PHP git tool.
    r = Tools::run(QStringLiteral("git_checkout"), QJsonObject{{"branch", QStringLiteral("nope")}});
    check(!r.ok, "a failed git command reports failure, not success-with-error-text");

    // ---- background shells ----
    r = Tools::run(QStringLiteral("bg"),
                   QJsonObject{{"command", QStringLiteral("echo alpha; sleep 1; echo omega")}});
    check(r.ok && r.output.contains(QStringLiteral("bg_")), "bg starts a job and returns at once");
    QString id;
    for (const QString& w : r.output.split(QRegularExpression(QStringLiteral("[\\s(]")))) {
        if (w.startsWith(QStringLiteral("bg_"))) {
            id = w;
            break;
        }
    }
    check(!id.isEmpty(), "bg hands back a job id");

    r = Tools::run(QStringLiteral("wait_bg"), QJsonObject{{"bg_id", id}, {"seconds", 20}});
    check(r.ok && r.output.contains(QStringLiteral("finished")), "wait_bg blocks until the job ends");

    r = Tools::run(QStringLiteral("bash_output"), QJsonObject{{"bg_id", id}});
    check(r.ok && r.output.contains(QStringLiteral("alpha")) &&
              r.output.contains(QStringLiteral("omega")),
          "bash_output returns the job's output");
    // The cursor: a second read shows only what is NEW, and the job is done, so
    // that is nothing at all.
    r = Tools::run(QStringLiteral("bash_output"), QJsonObject{{"bg_id", id}});
    check(r.ok && r.output.contains(QStringLiteral("no new output")),
          "bash_output only ever shows what you have not already seen");

    r = Tools::run(QStringLiteral("bash_output"), QJsonObject{{"bg_id", QStringLiteral("../../etc/passwd")}});
    check(!r.ok, "a job id cannot traverse out of the bg directory");

    // ---- calc ----
    r = Tools::run(QStringLiteral("calc"), QJsonObject{{"expr", QStringLiteral("2 + 3 * 4")}});
    check(r.ok && r.output == QStringLiteral("14"), "calc respects precedence");
    r = Tools::run(QStringLiteral("calc"), QJsonObject{{"expr", QStringLiteral("(1 + 2) ^ 3")}});
    check(r.ok && r.output == QStringLiteral("27"), "calc does parens and powers");
    r = Tools::run(QStringLiteral("calc"), QJsonObject{{"expr", QStringLiteral("-4 + 10")}});
    check(r.ok && r.output == QStringLiteral("6"), "calc does unary minus");
    // Both of these KILLED the PHP process: eval() raised a fatal Error that its
    // catch(Exception) never saw.
    r = Tools::run(QStringLiteral("calc"), QJsonObject{{"expr", QStringLiteral("1/0")}});
    check(!r.ok && r.error.contains(QStringLiteral("zero")), "calc survives division by zero");
    r = Tools::run(QStringLiteral("calc"), QJsonObject{{"expr", QStringLiteral("1 +")}});
    check(!r.ok, "calc survives a malformed expression");

    // ---- ask_user ----
    // Non-interactive: there is nobody to answer, so it must return AT ONCE with a
    // usable instruction rather than blocking the run forever.
    r = Tools::run(QStringLiteral("ask_user"),
                   QJsonObject{{"question", QStringLiteral("which one?")}});
    check(r.ok && r.output.contains(QStringLiteral("not interactive")),
          "ask_user never blocks a run that has nobody to ask");

    Tools::setThreadRoot(QString());
}

// Vision: an @image token becomes base64 on the message and LEAVES the prompt.
// Every surface that takes typed text must go through Vision::attach — the
// one-shot path did not, and silently sent the model the literal path instead.
static void testVision() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        check(false, "temp dir for the vision test");
        return;
    }
    // A 1x1 PNG. Real bytes, so encodeBase64 has something true to do.
    static const unsigned char png[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
        0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f,
        0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x00,
        0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x49,
        0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    const QString shot = tmp.path() + QStringLiteral("/shot.png");
    QFile f(shot);
    f.open(QIODevice::WriteOnly);
    f.write(reinterpret_cast<const char*>(png), sizeof(png));
    f.close();

    ChatMessage m;
    int attached = 0;
    QString cleaned = Vision::attach(m, QStringLiteral("what is wrong with @") + shot, &attached);
    check(attached == 1 && m.images.size() == 1, "an @image token attaches the image");
    check(m.images.value(0).startsWith(QStringLiteral("iVBORw0KGgo")),
          "the image is attached as base64 (a PNG header)");
    check(!cleaned.contains(shot), "the path is taken back OUT of the prompt the model reads");
    check(cleaned.contains(QStringLiteral("what is wrong with")), "the rest of the prompt survives");

    // /image is the other spelling, and a bare one still needs an instruction.
    ChatMessage m2;
    cleaned = Vision::attach(m2, QStringLiteral("/image ") + shot, &attached);
    check(m2.images.size() == 1 && !cleaned.isEmpty(),
          "/image attaches, and a bare /image still asks the model something");

    // A non-image @token is a file mention, NOT a picture — leave it alone.
    ChatMessage m3;
    const QString notes = tmp.path() + QStringLiteral("/notes.txt");
    QFile n(notes);
    n.open(QIODevice::WriteOnly);
    n.write("hi");
    n.close();
    cleaned = Vision::attach(m3, QStringLiteral("read @") + notes, &attached);
    check(m3.images.isEmpty() && cleaned.contains(notes),
          "a non-image @mention is left for the file-mention path");
}

// Live steering. Crew::steer wrote steer.jsonl and NOTHING read it back — the
// board's steer box, the CLI's `crew steer`, and the desktop all wrote into a
// file no coder ever opened. The fix is a consumer in the coder loop, so the
// guard is: the writer and the reader must both exist, and the reader must run
// BETWEEN iterations (never mid-tool, or a coder gets interrupted between
// deciding to write a file and writing it).
static void testCrewSteer() {
    const QString crew = readSource(QStringLiteral("core/Crew.cpp"));
    check(crew.contains("drainSteer"), "the coder loop actually READS steer.jsonl");
    check(crew.contains("steerSeen") && crew.contains("f.seek(steerSeen)"),
          "each coder remembers how far it read, so a steer is delivered exactly once");
    check(crew.contains("target == 0 || target == n"),
          "a coder takes messages addressed to it, or to the whole crew (target 0)");
    check(crew.contains("sink, cancel, drainSteer"),
          "the drain is wired into Agent::loop as the interject hook");

    const QString agent = readSource(QStringLiteral("core/Agent.cpp"));
    check(agent.contains("const Interject& interject") && agent.contains("interject()"),
          "Agent::loop takes a word from the human between iterations");
    check(agent.contains("sink.onTool"),
          "the agent reports each tool ACTION, which is what a live pane displays");

    // coderLog is the pane's feed: it must tail by offset, and it must survive a
    // log that shrank (a new run reusing the id) rather than slicing past the end.
    QTemporaryDir tmp;
    const QString runId = QStringLiteral("crew_testlog");
    const QString dir = Config::crewDir() + QStringLiteral("/") + runId;
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/coder-1.log");
    {
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write("hello");
        f.close();
    }
    qint64 size = 0;
    QString chunk = Crew::coderLog(runId, 1, 0, &size);
    check(chunk == QStringLiteral("hello") && size == 5, "coderLog reads from the start");
    chunk = Crew::coderLog(runId, 1, size, &size);
    check(chunk.isEmpty(), "coderLog returns nothing new when nothing was written");
    {
        QFile f(path);
        f.open(QIODevice::Append);
        f.write(" world");
        f.close();
    }
    chunk = Crew::coderLog(runId, 1, 5, &size);
    check(chunk == QStringLiteral(" world"), "coderLog returns only the DELTA");
    chunk = Crew::coderLog(runId, 1, 9999, &size);
    check(chunk == QStringLiteral("hello world"),
          "an offset past the end restarts rather than slicing past it");
    check(Crew::coderLog(QStringLiteral("nope"), 1, 0, &size).isEmpty(),
          "coderLog of a run that never started is empty, not a crash");
    QDir(dir).removeRecursively();
}

// Workspaces read and write the SAME ~/.ollamadev/workspaces.json the PHP app
// uses, which means the two can be run side by side during the port. The blob the
// desktop stores in `state` (canvas layout, terminal scrollback — tens of KB of
// it) is opaque to core, and a CLI `ws add` must never wipe it.
//
// HOME is redirected at the top so this never touches the real file.
static void testWorkspaces() {
    const QByteArray realHome = qgetenv("HOME");
    QTemporaryDir home;
    if (!home.isValid()) {
        check(false, "temp HOME for the workspace test");
        return;
    }
    qputenv("HOME", home.path().toUtf8());

    QTemporaryDir projA, projB;
    const Workspace a = Workspaces::add(projA.path(), QStringLiteral("alpha"));
    Workspaces::add(projB.path(), QStringLiteral("beta"));
    check(a.id.startsWith(QStringLiteral("ws_")), "a workspace id is derived from its path");
    check(Workspaces::all().size() == 2, "two workspaces are tracked");
    check(Workspaces::activeId() != a.id, "the most recently added one is active");

    // The desktop saves its canvas here. A later `ws add` on the same folder must
    // not clobber it — that would silently lose the user's window layout.
    const QJsonObject layout{{"panes", 3}, {"zoom", QStringLiteral("100%")}};
    check(Workspaces::saveState(a.id, layout), "the desktop can save its layout");
    Workspaces::add(projA.path(), QStringLiteral("alpha-renamed"));
    Workspace back;
    check(Workspaces::find(QStringLiteral("alpha-renamed"), &back), "re-adding renames in place");
    check(Workspaces::all().size() == 2, "re-adding the same path does NOT duplicate it");
    check(back.state == layout, "re-adding preserves the desktop's saved layout");

    check(Workspaces::find(QStringLiteral("BETA"), &back), "a workspace is found by name, any case");
    check(Workspaces::find(projB.path(), &back), "…and by path");
    check(Workspaces::find(back.id, &back), "…and by id");

    check(Workspaces::open(QStringLiteral("alpha-renamed")) == QFileInfo(projA.path()).canonicalFilePath(),
          "open prints the path (the shell does the cd — a child cannot chdir its parent)");
    check(Workspaces::activeId() == a.id, "open makes it active");

    // Removing the ACTIVE workspace must not leave `active` dangling.
    check(Workspaces::remove(QStringLiteral("alpha-renamed")), "remove works");
    check(Workspaces::all().size() == 1, "…and it is gone");
    check(!Workspaces::activeId().isEmpty() && Workspaces::activeId() != a.id,
          "removing the active workspace hands active to a survivor");
    check(!Workspaces::remove(QStringLiteral("nope")), "removing an unknown workspace fails cleanly");

    qputenv("HOME", realHome);
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
    s << "crew-resume\n"; s.flush(); testCrewResume();
    s << "crew-amplify\n"; s.flush(); testCrewAmplify();
    s << "agent-tools\n"; s.flush(); testAgentTools();
    s << "vision\n";      s.flush(); testVision();
    s << "crew-steer\n";  s.flush(); testCrewSteer();
    s << "workspaces\n"; s.flush(); testWorkspaces();

    s << "\n" << passed << " passed, " << failed << " failed\n";
    s.flush();
    return failed == 0 ? 0 : 1;
}
