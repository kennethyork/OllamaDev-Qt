#include "Eval.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include "Agent.h"
#include "Backend.h"
#include "Config.h"
#include "Json.h"
#include "Models.h"
#include "Parallel.h"
#include "Sandbox.h"
#include "Tools.h"

namespace odv {
namespace {

// Admission key for the limiter. These STRINGS are the contract: the semaphore is
// keyed by string, so producing the same key Crew.cpp produces
// (core/Crew.cpp::limiterKey) is what makes an eval sweep and a running crew share
// one pool of local GPU slots instead of each opening its own and oversubscribing
// the same hardware. Keep the two in step.
QString limiterKey(const QString& backendId, const QString& model) {
    if (backendId == QLatin1String("ollama")) {
        return Models::isCloud(model) ? QStringLiteral("ollama:cloud")
                                      : QStringLiteral("ollama:local");
    }
    return backendId;
}

// The interpreters a check may ask for. Resolved from PATH once, because a check
// that shells out to a bare `python` name would be at the mercy of whatever the
// task's temp dir happens to contain.
QString interpreter(const QString& placeholder) {
    if (placeholder == QLatin1String("python")) {
        // python3 first: on most Linux boxes plain `python` does not exist at all,
        // which is exactly the trap the PHP original fell into.
        const QString p3 = QStandardPaths::findExecutable(QStringLiteral("python3"));
        if (!p3.isEmpty()) return p3;
        return QStandardPaths::findExecutable(QStringLiteral("python"));
    }
    if (placeholder == QLatin1String("php"))
        return QStandardPaths::findExecutable(QStringLiteral("php"));
    return {};
}

// Shell-quote a resolved interpreter path (it can contain spaces on Windows/macOS).
QString shellQuote(const QString& s) {
    QString q = s;
    q.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

// Substitute {python}/{php} in a check command. Returns false and names the
// missing one when the machine has no such interpreter.
bool expandPlaceholders(QString* cmd, QString* missing) {
    static const QRegularExpression ph(QStringLiteral("\\{(python|php)\\}"));
    auto it = ph.globalMatch(*cmd);
    QStringList wanted;
    while (it.hasNext()) {
        const QString name = it.next().captured(1);
        if (!wanted.contains(name)) wanted << name;
    }
    for (const QString& name : wanted) {
        const QString path = interpreter(name);
        if (path.isEmpty()) {
            *missing = name;
            return false;
        }
        cmd->replace(QLatin1Char('{') + name + QLatin1Char('}'), shellQuote(path));
    }
    return true;
}

struct RunOut {
    int exitCode = -1;
    QString text;
    bool timedOut = false;
};

// A check's command IS a shell string (it carries its own quoting, e.g.
// `python -c "…"`), so it goes through sh -c — but with an explicit working
// directory, never the process cwd: eval tasks run concurrently on several
// threads and the process cwd is one shared thing they would all fight over.
RunOut runCheckCmd(const QString& cmd, const QString& dir, int timeoutMs) {
    RunOut r;
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.setWorkingDirectory(dir);
#ifdef Q_OS_WIN
    p.start(QStringLiteral("cmd"), {QStringLiteral("/C"), cmd});
#else
    p.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), cmd});
#endif
    if (!p.waitForStarted(5000)) {
        r.text = QStringLiteral("failed to start shell");
        return r;
    }
    if (!p.waitForFinished(timeoutMs)) {
        r.timedOut = true;
        // Our own child, terminated by handle. Never a kill by name.
        p.terminate();
        if (!p.waitForFinished(2000)) p.kill();
        r.text = QString::fromUtf8(p.readAll()).trimmed();
        return r;
    }
    r.text = QString::fromUtf8(p.readAll()).trimmed();
    r.exitCode = p.exitCode();
    return r;
}

EvalTask task(const QString& name, const QString& prompt, const QJsonObject& check,
              const QMap<QString, QString>& files = {}) {
    EvalTask t;
    t.name = name;
    t.prompt = prompt;
    t.check = check;
    t.files = files;
    return t;
}

QJsonObject fileContains(const QString& path, const QString& needle, bool normalize = false) {
    QJsonObject c{{"type", "file_contains"}, {"path", path}, {"needle", needle}};
    if (normalize) c.insert(QStringLiteral("normalize"), true);
    return c;
}

QJsonObject command(const QString& cmd, const QString& expect = {}) {
    QJsonObject c{{"type", "command"}, {"cmd", cmd}};
    if (!expect.isEmpty()) c.insert(QStringLiteral("expect"), expect);
    return c;
}

QString safeName(const QString& s) {
    QString out;
    for (const QChar& c : s) out += (c.isLetterOrNumber() || c == '-' || c == '_') ? c : QChar('-');
    return out;
}

}  // namespace

// ------------------------------------------------------------------ the suite

// The built-in suite.
//
// PORTED TO PYTHON. The PHP original checked every code task by shelling out to
// PHP_BINARY — which the C++ app no longer ships, so on a machine without PHP the
// whole suite would have measured nothing. python3 is the one interpreter that is
// essentially always there; {php} still works for user tasks, and any task whose
// interpreter is missing is skipped rather than counted as a model failure.
//
// Raw strings use a custom delimiter: these commands end in `')"` and the default
// )" delimiter would terminate the literal in the middle of the shell quoting.
QVector<EvalTask> Evals::builtins() {
    QVector<EvalTask> t;

    // ---- files / editing ---------------------------------------------------
    t << task("create-file",
              "Create a file named greeting.txt containing exactly this text and nothing else: "
              "hello world",
              fileContains("greeting.txt", "hello world"));

    t << task("edit-json",
              "In settings.json, change the value of \"debug\" from false to true. Leave "
              "everything else unchanged.",
              fileContains("settings.json", "\"debug\":true", true),
              {{"settings.json", "{\n  \"debug\": false,\n  \"name\": \"demo\"\n}\n"}});

    t << task("edit-two",
              "In conf.ini, change port to 8080 and change debug to on. Leave host unchanged.",
              command(R"py({python} -c "c=open('conf.ini').read(); )py"
                      R"py(print('OK' if 'port=8080' in c and 'debug=on' in c else 'NO')")py",
                      "OK"),
              {{"conf.ini", "host=localhost\nport=3000\ndebug=off\n"}});

    t << task("make-app",
              "Create a tiny static web page: index.html with an <h1>Hello</h1> that links a "
              "stylesheet style.css, and style.css that sets the body background to black. Two "
              "files.",
              command(R"py({python} -c "import os; )py"
                      R"py(print('OK' if os.path.isfile('index.html') and )py"
                      R"py(os.path.isfile('style.css') and )py"
                      R"py('style.css' in open('index.html').read() else 'NO')")py",
                      "OK"));

    t << task("gitignore",
              "Create a .gitignore file that ignores the node_modules directory and all .log "
              "files.",
              command(R"py({python} -c "g=open('.gitignore').read(); )py"
                      R"py(print('OK' if 'node_modules' in g and '.log' in g else 'NO')")py",
                      "OK"));

    t << task("readme",
              "Create README.md with a level-1 heading for the project title (a line starting "
              "with \"# \") and a section titled \"## Usage\".",
              command(R"py({python} -c "import re; r=open('README.md').read(); )py"
                      R"py(print('OK' if re.search('^# ', r, re.M) and '## Usage' in r )py"
                      R"py(else 'NO')")py",
                      "OK"));

    // ---- algorithms / functions -------------------------------------------
    t << task("new-function",
              "Create a Python file named strutil.py defining a function slugify(s: str) -> str "
              "that returns the input lowercased with runs of spaces replaced by single hyphens. "
              "The file must print nothing when imported.",
              command(R"py({python} -c "import strutil; print(strutil.slugify('Hello World'))")py",
                      "hello-world"));

    t << task("fizzbuzz",
              "Create fizzbuzz.py defining fizzbuzz(n: int) -> str - return \"FizzBuzz\" if n is "
              "divisible by 15, \"Fizz\" if by 3, \"Buzz\" if by 5, otherwise the number as a "
              "string. It must print nothing when imported.",
              command(R"py({python} -c "import fizzbuzz as f; print(','.join([f.fizzbuzz(15), )py"
                      R"py(f.fizzbuzz(3), f.fizzbuzz(5), f.fizzbuzz(7)]))")py",
                      "FizzBuzz,Fizz,Buzz,7"));

    t << task("factorial",
              "Create fact.py defining factorial(n: int) -> int (with 0! = 1). It must print "
              "nothing when imported.",
              command(R"py({python} -c "import fact; print(fact.factorial(5))")py", "120"));

    t << task("palindrome",
              "Create pal.py defining is_palindrome(s: str) -> bool - True if the string reads "
              "the same backward, case-insensitive. It must print nothing when imported.",
              command(R"py({python} -c "import pal; print('OK' if pal.is_palindrome('Racecar') )py"
                      R"py(and not pal.is_palindrome('hello') else 'NO')")py",
                      "OK"));

    t << task("word-count",
              "Create wordcount.py defining word_count(s: str) -> int - the number of "
              "whitespace-separated words. It must print nothing when imported.",
              command(R"py({python} -c "import wordcount; )py"
                      R"py(print(wordcount.word_count('the quick brown fox'))")py",
                      "4"));

    t << task("celsius",
              "Create temps.py defining c_to_f(c: float) -> float converting Celsius to "
              "Fahrenheit. It must print nothing when imported.",
              command(R"py({python} -c "import temps; print(int(temps.c_to_f(100)))")py", "212"));

    t << task("dedup",
              "Create dedup.py defining dedup(a: list) -> list - remove duplicate values while "
              "preserving first-occurrence order. It must print nothing when imported.",
              command(R"py({python} -c "import dedup; )py"
                      R"py(print(','.join(str(x) for x in dedup.dedup([1,2,2,3,1])))")py",
                      "1,2,3"));

    t << task("binary-search",
              "Create bsearch.py defining bsearch(items: list, target: int) -> int - return the "
              "index of target in the ascending-sorted list using binary search, or -1 if absent. "
              "It must print nothing when imported.",
              command(R"py({python} -c "import bsearch as b; )py"
                      R"py(print(str(b.bsearch([1,3,5,7,9,11],7)) + ',' + )py"
                      R"py(str(b.bsearch([1,3,5,7,9,11],4)))")py",
                      "3,-1"));

    t << task("fib-memo",
              "Create fib.py defining fib(n: int) -> int returning the nth Fibonacci number "
              "(fib(0)=0, fib(1)=1). It must compute fib(30) fast - use iteration or memoization, "
              "not naive recursion. It must print nothing when imported.",
              command(R"py({python} -c "import fib; print(fib.fib(30))")py", "832040"));

    // ---- parsing / data / classes -----------------------------------------
    // The \n inside these is deliberate: sh keeps a backslash literal inside double
    // quotes (it is only special before $ ` " \ or a newline), so python receives
    // the two characters and turns them into a real newline in its own source.
    t << task("env-parser",
              "Create envparse.py defining parse_env(s: str) -> dict - parse lines of KEY=VALUE "
              "into a dictionary, skipping blank lines. It must print nothing when imported.",
              command(R"py({python} -c "import envparse; )py"
                      R"py(print(envparse.parse_env('A=1\nB=2')['B'])")py",
                      "2"));

    t << task("csv-parse",
              "Create csvutil.py defining parse_csv(s: str) -> list - split the text into rows by "
              "newline and each row into fields by comma, skipping blank lines. Returns a list of "
              "lists. It must print nothing when imported.",
              command(R"py({python} -c "import csvutil; )py"
                      R"py(print(csvutil.parse_csv('a,b,c\n1,2,3')[1][2])")py",
                      "3"));

    t << task("stack-class",
              "Create stack.py defining a class Stack with push(v) and pop() behaving as a LIFO "
              "stack (pop returns the most recently pushed value). It must print nothing when "
              "imported.",
              command(R"py({python} -c "from stack import Stack; s=Stack(); s.push(1); )py"
                      R"py(s.push(2); print(s.pop())")py",
                      "2"));

    t << task("bank-class",
              "Create account.py defining a class Account that starts with balance 0 and has "
              "deposit(n), withdraw(n) which must NOT let the balance go negative (ignore an "
              "over-withdraw), and balance() returning the current balance. It must print nothing "
              "when imported.",
              command(R"py({python} -c "from account import Account; a=Account(); )py"
                      R"py(a.deposit(100); a.withdraw(30); a.withdraw(1000); print(a.balance())")py",
                      "70"));

    // ---- multi-file --------------------------------------------------------
    t << task("module",
              "Create lib/greet.py defining greet(name: str) -> str returning \"Hello, \" "
              "followed by the name, and main.py that imports it from lib.greet and prints "
              "greet(\"World\").",
              command(R"py({python} main.py)py", "Hello, World"));

    t << task("refactor-extract",
              "Refactor app.py: MOVE the greet() function out into a new file helpers.py, and "
              "make app.py import greet from helpers instead of defining it. Running app.py must "
              "still print \"Hi A\".",
              command(R"py({python} -c "a=open('app.py').read(); h=open('helpers.py').read(); )py"
                      R"py(print('OK' if 'def greet' in h and 'def greet' not in a and )py"
                      R"py('helpers' in a else 'NO')")py",
                      "OK"),
              {{"app.py", "def greet(n):\n    return 'Hi ' + n\n\n\nprint(greet('A'))\n"}});

    // ---- seeded bug fixes --------------------------------------------------
    t << task("fix-bug",
              "calc.py has a bug: add() subtracts instead of adding. Fix add() so it returns the "
              "sum of its two arguments.",
              command(R"py({python} -c "import calc; print(calc.add(2,3))")py", "5"),
              {{"calc.py", "def add(a, b):\n    return a - b\n"}});

    t << task("fix-loop",
              "sum_to(n) in sumto.py is off by one - it should return the sum of 1..n inclusive "
              "but currently excludes n. Fix it.",
              command(R"py({python} -c "import sumto; print(sumto.sum_to(5))")py", "15"),
              {{"sumto.py",
                "def sum_to(n):\n    s = 0\n    for i in range(1, n):\n        s += i\n    return s\n"}});

    t << task("fix-return",
              "multiply() in mul.py adds instead of multiplying. Fix it to return the product.",
              command(R"py({python} -c "import mul; print(mul.multiply(6,7))")py", "42"),
              {{"mul.py", "def multiply(a, b):\n    return a + b\n"}});

    t << task("fix-syntax",
              "broken.py has a Python syntax error. Fix it so the file compiles cleanly. Do not "
              "change what f() returns.",
              // No `expect`: a clean exit from the compiler IS the verdict.
              command(R"py({python} -m py_compile broken.py)py"),
              {{"broken.py", "def f(:\n    return 1\n"}});

    t << task("fix-two-bugs",
              "shape.py has TWO bugs: area() should return width*height (it adds), and "
              "perimeter() should return 2*(width+height) (it multiplies). Fix both functions.",
              command(R"py({python} -c "import shape; )py"
                      R"py(print(str(shape.area(3,4)) + ',' + str(shape.perimeter(3,4)))")py",
                      "12,14"),
              {{"shape.py",
                "def area(w, h):\n    return w + h\n\n\ndef perimeter(w, h):\n    return w * h\n"}});

    return t;
}

QStringList Evals::userDirs() {
    return {QDir::currentPath() + QStringLiteral("/evals"),
            Config::dataDir() + QStringLiteral("/evals")};
}

QVector<EvalTask> Evals::suite(const QString& only) {
    QVector<EvalTask> tasks = builtins();

    for (const QString& dir : userDirs()) {
        QDir d(dir);
        if (!d.exists()) continue;
        for (const QFileInfo& fi : d.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
            QFile f(fi.absoluteFilePath());
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QJsonDocument doc = json::decodeLoose(QString::fromUtf8(f.readAll()));

            // A file may hold one task object or an array of them.
            QJsonArray entries;
            if (doc.isArray()) {
                entries = doc.array();
            } else if (doc.isObject()) {
                entries.append(doc.object());
            }

            for (const QJsonValue& v : entries) {
                const QJsonObject o = v.toObject();
                EvalTask t;
                t.name = o.value(QStringLiteral("name")).toString();
                t.prompt = o.value(QStringLiteral("prompt")).toString();
                t.check = o.value(QStringLiteral("check")).toObject();
                if (t.name.isEmpty() || t.prompt.isEmpty() || t.check.isEmpty()) continue;
                const QJsonObject files = o.value(QStringLiteral("files")).toObject();
                for (auto it = files.constBegin(); it != files.constEnd(); ++it)
                    t.files.insert(it.key(), it.value().toString());
                tasks << t;
            }
        }
    }

    if (!only.isEmpty()) {
        QVector<EvalTask> filtered;
        for (const EvalTask& t : tasks)
            if (t.name == only) filtered << t;
        return filtered;
    }
    return tasks;
}

// ------------------------------------------------------------------ the checks

bool Evals::check(const QJsonObject& c, const QString& dir, QString* detail, bool* skipped) {
    const QString type = c.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("file_exists") || type == QLatin1String("file_contains")) {
        const QString rel = c.value(QStringLiteral("path")).toString();
        const QString path = dir + QLatin1Char('/') + QString(rel).remove(QRegularExpression("^/+"));
        if (!QFileInfo(path).isFile()) {
            *detail = QStringLiteral("file missing: %1").arg(rel);
            return false;
        }
        if (type == QLatin1String("file_exists")) {
            *detail = QStringLiteral("file present");
            return true;
        }

        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            *detail = QStringLiteral("file unreadable: %1").arg(rel);
            return false;
        }
        QString hay = QString::fromUtf8(f.readAll());
        QString needle = c.value(QStringLiteral("needle")).toString();
        if (c.value(QStringLiteral("normalize")).toBool()) {
            static const QRegularExpression ws(QStringLiteral("\\s+"));
            hay.remove(ws);
            needle.remove(ws);
        }
        const bool ok = !needle.isEmpty() && hay.contains(needle);
        *detail = ok ? QStringLiteral("content matched")
                     : QStringLiteral("needle not found: %1")
                           .arg(c.value(QStringLiteral("needle")).toString());
        return ok;
    }

    if (type == QLatin1String("command")) {
        QString cmd = c.value(QStringLiteral("cmd")).toString();
        QString missing;
        if (!expandPlaceholders(&cmd, &missing)) {
            *skipped = true;
            *detail = QStringLiteral("skipped: no %1 on PATH").arg(missing);
            return false;
        }
        const RunOut r = runCheckCmd(cmd, dir, qMax(5, Config::integer("eval.checkTimeout", 30)) * 1000);
        const QString expect = c.value(QStringLiteral("expect")).toString();
        const bool ok = r.exitCode == 0 && (expect.isEmpty() || r.text.contains(expect));
        if (ok) {
            *detail = QStringLiteral("command ok");
            return true;
        }
        if (r.timedOut) {
            *detail = QStringLiteral("check timed out");
        } else if (expect.isEmpty()) {
            *detail = QStringLiteral("exit %1: %2").arg(r.exitCode).arg(r.text.left(120));
        } else {
            *detail = QStringLiteral("exit %1, wanted \"%2\", got \"%3\"")
                          .arg(r.exitCode)
                          .arg(expect, r.text.left(120));
        }
        return false;
    }

    *detail = QStringLiteral("unknown check type: %1").arg(type);
    return false;
}

// ------------------------------------------------------------------ the runner

EvalResult Evals::runOne(const EvalTask& t, const QString& backend, const QString& model,
                         bool keep) {
    EvalResult r;
    r.model = model;
    r.name = t.name;

    // Our own directory, named for this process, this model and this task — so two
    // concurrent tasks cannot land in the same tree, and cleanup only ever removes
    // the one path we created. Never a wipe of a shared parent.
    const QString dir = QDir::tempPath() + QStringLiteral("/ollamadev-eval/%1-%2-%3")
                                               .arg(QCoreApplication::applicationPid())
                                               .arg(safeName(model), safeName(t.name));
    Sandbox::removeTree(dir);
    if (!QDir().mkpath(dir)) {
        r.detail = QStringLiteral("could not create %1").arg(dir);
        return r;
    }

    for (auto it = t.files.constBegin(); it != t.files.constEnd(); ++it) {
        const QString path = dir + QLatin1Char('/') + it.key();
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(it.value().toUtf8());
    }

    QElapsedTimer clock;
    clock.start();

    // The isolation boundary. thread-local, NOT a chdir: tasks run concurrently and
    // a process-wide cwd would have them writing into each other's temp dirs. This
    // also confines the agent's tools to the task dir — resolvePath() rejects any
    // path that escapes an explicitly-set root.
    const QString prevRoot = Tools::hasThreadRoot() ? Tools::threadRoot() : QString();
    Tools::setThreadRoot(dir);

    Agent a(backend, model);
    QVector<ChatMessage> msgs{{"system", a.buildSystemPrompt(dir), {}, {}, {}, {}, {}},
                              {"user", t.prompt, {}, {}, {}, {}, {}}};

    // No StreamSink: an eval prints a table, not a transcript. This is what the PHP
    // needed ob_start() for.
    StreamSink sink;
    CancelToken cancel;
    a.loop(msgs, Config::integer("eval.maxIterations", 20), sink, cancel);

    Tools::setThreadRoot(prevRoot);

    QString detail;
    bool skipped = false;
    r.pass = check(t.check, dir, &detail, &skipped);
    r.skipped = skipped;
    r.detail = detail;
    r.ms = static_cast<int>(clock.elapsed());

    if (keep && !r.pass) {
        r.detail += QStringLiteral("  (kept: %1)").arg(dir);
    } else {
        Sandbox::removeTree(dir);
    }
    return r;
}

QVector<EvalResult> Evals::run(const QVector<EvalTask>& tasks, const QString& backend,
                               const QStringList& models, bool keep,
                               const std::function<void(const EvalResult&)>& onDone) {
    QVector<EvalResult> out;
    if (tasks.isEmpty() || models.isEmpty()) return out;

    // Every (model, task) pair is one job. Flattening the whole sweep into a single
    // pool — rather than a per-model wave — is what lets a cloud model's tasks keep
    // running while the local model's tasks are parked waiting for a GPU slot.
    struct Job {
        int model;
        int task;
    };
    QVector<Job> jobs;
    for (int m = 0; m < models.size(); ++m)
        for (int i = 0; i < tasks.size(); ++i) jobs.push_back({m, i});

    // Cap each key at what the backend says it can really take. Same numbers the
    // crew uses, and — because the keys match — the same semaphores.
    {
        auto b = Backends::get(backend);
        QHash<QString, int> caps;
        for (const QString& m : models) {
            const QString key = limiterKey(backend, m);
            const int lim = b ? qMax(1, b->concurrencyLimit(m)) : 1;
            caps[key] = caps.contains(key) ? qMin(caps[key], lim) : lim;
        }
        for (auto it = caps.constBegin(); it != caps.constEnd(); ++it)
            Limiter::instance().setLimit(it.key(), it.value());
    }

    // Auto + non-interactive, process-wide: an eval task must never stop to ask a
    // question nobody is watching. Restored afterwards so a REPL that ran `eval`
    // does not silently keep auto-approving.
    const PermMode prevMode = Permission::mode();
    const bool prevInteractive = Permission::interactive();
    Permission::setMode(PermMode::Auto);
    Permission::setInteractive(false);

    QMutex reportLock;
    const QVector<QJsonObject> raw = parallelRun(
        jobs.size(),
        [&](int i) { return limiterKey(backend, models.at(jobs.at(i).model)); },
        [&](int i) -> QJsonObject {
            const Job& j = jobs.at(i);
            const EvalResult r = runOne(tasks.at(j.task), backend, models.at(j.model), keep);
            if (onDone) {
                // Jobs finish on N threads; the caller's printer is not thread-safe.
                QMutexLocker lock(&reportLock);
                onDone(r);
            }
            return QJsonObject{{"model", r.model}, {"name", r.name},   {"pass", r.pass},
                               {"skipped", r.skipped}, {"ms", r.ms},   {"detail", r.detail}};
        });

    Permission::setMode(prevMode);
    Permission::setInteractive(prevInteractive);

    out.reserve(raw.size());
    for (const QJsonObject& o : raw) {
        EvalResult r;
        r.model = o.value(QStringLiteral("model")).toString();
        r.name = o.value(QStringLiteral("name")).toString();
        r.pass = o.value(QStringLiteral("pass")).toBool();
        r.skipped = o.value(QStringLiteral("skipped")).toBool();
        r.ms = o.value(QStringLiteral("ms")).toInt();
        r.detail = o.value(QStringLiteral("detail")).toString();
        out << r;
    }
    return out;
}

}  // namespace odv
