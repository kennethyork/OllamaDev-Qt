#include "Verify.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>

#include "Agent.h"
#include "Config.h"
#include "Json.h"
#include "Tools.h"

namespace odv {
namespace {

// A test suite may legitimately take a long time; a wedged one must still not hang
// the agent forever.
constexpr int kTestTimeoutMs = 30 * 60 * 1000;

// How much failure output the model sees. The tail is where the assertion is.
constexpr int kFailureTailLines = 120;

// Tool-turns allowed in ONE fix attempt before we re-run the tests regardless. The
// re-run is the ground truth, so an agent that thinks it is done early is fine and
// an agent that will not stop is capped.
constexpr int kFixIterations = 12;

bool has(const QString& root, const QString& rel) {
    return QFileInfo::exists(QDir(root).filePath(rel));
}

QString readAll(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(f.readAll());
}

// A CMake project only has tests to run once it has been CONFIGURED — ctest reads
// CTestTestfile.cmake out of the build tree, not CMakeLists.txt out of the source.
// Pointing ctest at an unconfigured tree just fails, so we only claim the project
// when a real build dir exists.
QString configuredBuildDir(const QString& root) {
    for (const QString& d : {QStringLiteral("build"), QStringLiteral("Build"),
                             QStringLiteral("cmake-build-debug"), QStringLiteral("out/build")}) {
        if (has(root, d + QStringLiteral("/CTestTestfile.cmake"))) return d;
    }
    return {};
}

// Restores the permission mode on every exit path, including an exception out of
// the agent loop. A fix loop that left the session in Auto would silently disarm
// the approval prompt for everything the user did afterwards.
struct PermissionScope {
    PermMode mode = Permission::mode();
    bool interactive = Permission::interactive();
    PermissionScope() {
        // The agent is fixing tests unattended: there is nobody to answer a prompt,
        // and in Ask mode with no approver every edit would simply be denied.
        Permission::setMode(PermMode::Auto);
        Permission::setInteractive(false);
    }
    ~PermissionScope() {
        Permission::setMode(mode);
        Permission::setInteractive(interactive);
    }
};

}  // namespace

std::optional<TestCommand> Verify::detect(const QString& root) {
    const QString override_ = Config::str(QStringLiteral("test.command")).trimmed();
    if (!override_.isEmpty()) return TestCommand{override_, QStringLiteral("config")};

    // CMake FIRST, and specifically before the pytest rule: a C++ project routinely
    // has a tests/ directory, and "there is a tests/ dir" is all pytest needs to
    // match. Checked in this order, a CMake repo is never mistaken for a Python one.
    if (has(root, QStringLiteral("CMakeLists.txt"))) {
        const QString bd = configuredBuildDir(root);
        // Flags verified against `ctest --help`: --test-dir <dir>, --output-on-failure.
        if (!bd.isEmpty())
            return TestCommand{QStringLiteral("ctest --test-dir %1 --output-on-failure").arg(bd),
                               QStringLiteral("ctest")};
    }

    if (has(root, QStringLiteral("phpunit.xml")) || has(root, QStringLiteral("phpunit.xml.dist"))) {
        const bool vendored = has(root, QStringLiteral("vendor/bin/phpunit"));
        return TestCommand{vendored ? QStringLiteral("./vendor/bin/phpunit")
                                    : QStringLiteral("phpunit"),
                           QStringLiteral("phpunit")};
    }

    if (has(root, QStringLiteral("composer.json"))) {
        const QJsonObject cj =
            json::objectFrom(readAll(QDir(root).filePath(QStringLiteral("composer.json"))));
        if (!json::at(cj, QStringLiteral("scripts.test")).isUndefined())
            return TestCommand{QStringLiteral("composer test"), QStringLiteral("composer")};
    }

    if (has(root, QStringLiteral("go.mod")))
        return TestCommand{QStringLiteral("go test ./..."), QStringLiteral("go")};

    if (has(root, QStringLiteral("Cargo.toml")))
        return TestCommand{QStringLiteral("cargo test"), QStringLiteral("cargo")};

    const bool pyMarker = has(root, QStringLiteral("pytest.ini")) ||
                          has(root, QStringLiteral("tox.ini")) ||
                          has(root, QStringLiteral("pyproject.toml")) ||
                          has(root, QStringLiteral("setup.cfg"));
    // A bare tests/ dir only means pytest if there is actually Python in it. The PHP
    // original matched on the directory alone and cheerfully claimed every C++ repo.
    const bool pyTests =
        !QDir(QDir(root).filePath(QStringLiteral("tests")))
             .entryList({QStringLiteral("*.py")}, QDir::Files)
             .isEmpty();
    if (pyMarker || pyTests)
        return TestCommand{QStringLiteral("pytest -q"), QStringLiteral("pytest")};

    if (has(root, QStringLiteral("package.json"))) {
        const QJsonObject pj =
            json::objectFrom(readAll(QDir(root).filePath(QStringLiteral("package.json"))));
        if (!json::at(pj, QStringLiteral("scripts.test")).isUndefined())
            return TestCommand{QStringLiteral("npm test"), QStringLiteral("npm")};
    }

    const bool mk = has(root, QStringLiteral("Makefile"));
    if (mk || has(root, QStringLiteral("makefile"))) {
        const QString text = readAll(QDir(root).filePath(
            mk ? QStringLiteral("Makefile") : QStringLiteral("makefile")));
        static const QRegularExpression target(QStringLiteral("^test:"),
                                               QRegularExpression::MultilineOption);
        if (target.match(text).hasMatch())
            return TestCommand{QStringLiteral("make test"), QStringLiteral("make")};
    }

    return std::nullopt;
}

TestRun Verify::run(const TestCommand& t, const std::function<void(const QString&)>& onOutput,
                    const CancelToken& cancel) {
    TestRun r;
    r.cmd = t.cmd;
    r.label = t.label;

    QProcess p;
    // A test command is a command LINE, not a program plus argv: `npm test`,
    // `go test ./...`, and above all a user's `test.command` override may contain
    // pipes or &&. So a shell is the correct interpreter here — but the command is
    // passed as ONE argv element, never spliced into a string we built, so there is
    // no place for an injected fragment to appear.
    p.setProgram(QStringLiteral("sh"));
    p.setArguments({QStringLiteral("-c"), t.cmd});
    // Tests are a subprocess that reads (and, via the fix loop, whose failures cause
    // us to write) the project. A crew coder runs on its own thread with its own
    // sandbox root, so running in the process cwd would test the USER'S tree instead
    // of the sandbox it is supposed to be confined to.
    p.setWorkingDirectory(Tools::threadRoot());
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start();
    if (!p.waitForStarted(10000)) {
        r.output = QStringLiteral("could not start: %1").arg(t.cmd);
        return r;
    }

    QElapsedTimer clock;
    clock.start();
    while (p.state() != QProcess::NotRunning) {
        if (cancel.cancelled() || clock.hasExpired(kTestTimeoutMs)) {
            p.terminate();  // our own child, by handle
            if (!p.waitForFinished(3000)) p.kill();
            r.output += cancel.cancelled() ? QStringLiteral("\n[cancelled]\n")
                                           : QStringLiteral("\n[timed out]\n");
            r.exit = 1;
            return r;
        }
        p.waitForReadyRead(200);
        const QString chunk = QString::fromUtf8(p.readAll());
        if (!chunk.isEmpty()) {
            r.output += chunk;
            if (onOutput) onOutput(chunk);
        }
    }
    const QString tail = QString::fromUtf8(p.readAll());
    if (!tail.isEmpty()) {
        r.output += tail;
        if (onOutput) onOutput(tail);
    }

    r.exit = p.exitCode();
    return r;
}

int Verify::fixLoop(const TestCommand& t, int maxAttempts, const QString& backendId,
                    const QString& model, const VerifyEvents& ev, const CancelToken& cancel) {
    if (maxAttempts < 1) maxAttempts = 1;

    const BackendPtr be = Backends::get(backendId.isEmpty() ? QStringLiteral("ollama") : backendId);
    if (!be || !be->available()) return 1;

    const PermissionScope scope;
    Agent agent(backendId, model);

    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        if (cancel.cancelled()) return 1;

        const TestRun res = run(t, ev.onOutput, cancel);
        if (ev.onAttempt) ev.onAttempt(attempt, maxAttempts, res.green());
        if (res.green()) return 0;
        if (attempt == maxAttempts || cancel.cancelled()) break;

        // Only the tail: a failing suite prints megabytes, and the assertion that
        // actually explains the failure is at the bottom of it.
        const QStringList lines = res.output.split('\n');
        const QString tail = lines.mid(qMax(0, lines.size() - kFailureTailLines)).join('\n');

        if (ev.onFixStart) ev.onFixStart();

        ChatMessage sys;
        sys.role = QStringLiteral("system");
        sys.content =
            QStringLiteral(
                "You are fixing FAILING TESTS. Read the failure output, find the root cause with "
                "your tools (view, grep, code_search), and EDIT the code so `%1` passes. Make "
                "minimal, correct changes. Do NOT weaken, skip, or delete tests just to make them "
                "pass. Actually call your tools to make the edits — do not merely describe them. "
                "Stop when you believe the fix is complete.")
                .arg(t.cmd);

        ChatMessage user;
        user.role = QStringLiteral("user");
        user.content = QStringLiteral("Test command: %1\n\nFailure output (tail):\n%2")
                           .arg(t.cmd, tail);

        QVector<ChatMessage> msgs{sys, user};
        StreamSink sink;
        if (ev.onFixStep) sink.onContent = [&ev](const QString&) { ev.onFixStep(); };
        agent.loop(msgs, kFixIterations, sink, cancel);
    }

    return 1;
}

}  // namespace odv
