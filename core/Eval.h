#pragma once
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

namespace odv {

// One eval task. `files` seeds the isolated working dir before the agent runs;
// `check` is the deterministic verdict (see Evals::check).
struct EvalTask {
    QString name;
    QString prompt;
    QMap<QString, QString> files;  // relative path -> contents
    QJsonObject check;
};

struct EvalResult {
    QString model;
    QString name;
    bool pass = false;
    // The check needs an interpreter this machine does not have. Skipped tasks are
    // excluded from the denominator: a missing python is not the model's failure,
    // and counting it as one would make the pass rate a measure of the box, not
    // the model.
    bool skipped = false;
    int ms = 0;
    QString detail;
};

struct EvalOptions {
    QString only;          // run just this task
    QString backend;       // defaults to model.backend
    QString model;         // defaults to the backend's default model
    QStringList compare;   // run the suite once per model
    bool json = false;
    bool keep = false;     // leave the temp dir of a failed task behind
    double min = -1.0;     // pass-rate floor in %, <0 = no floor
};

// EVAL — a fixed task suite that turns "the agent works" into a number: the pass
// rate over N small, well-defined coding tasks. Every task runs ISOLATED in its
// own temp dir under auto-permission, and the verdict is deterministic (expected
// file content, or a command's exit code and output) — never an LLM judging
// another LLM, which measures agreeableness rather than correctness.
class Evals {
public:
    // The built-in suite. Python, not the PHP original's PHP: the port does not
    // ship a PHP runtime, and a task whose check cannot run is worthless. Tasks
    // whose interpreter is absent are skipped, not failed.
    static QVector<EvalTask> builtins();

    // Where user-authored *.json tasks live: <cwd>/evals and <cwd>/.ollamadev/evals.
    static QStringList userDirs();

    // Built-ins + user tasks, optionally filtered to one name.
    static QVector<EvalTask> suite(const QString& only = {});

    // Run one task to completion in its own temp dir. Blocking; thread-safe —
    // the working root is thread-local (Tools::setThreadRoot), which is the whole
    // reason tasks can run concurrently at all.
    static EvalResult runOne(const EvalTask& t, const QString& backend, const QString& model,
                             bool keep);

    // Run the suite across every model in `models`, CONCURRENTLY.
    //
    // The PHP ran tasks strictly one at a time. Here every (model, task) pair goes
    // through Limiter with the SAME admission key the crew uses
    // ("ollama:local" / "ollama:cloud" / the backend id), so an eval sweep queues
    // behind a running crew for the local GPU instead of fighting it for slots —
    // and a cloud sweep, which contends for nothing local, fans out wide.
    static QVector<EvalResult> run(const QVector<EvalTask>& tasks, const QString& backend,
                                   const QStringList& models, bool keep,
                                   const std::function<void(const EvalResult&)>& onDone = {});

    // Deterministic verdict. Returns pass; `detail` explains a failure, and
    // `skipped` is set when the check needs an interpreter that is not installed.
    static bool check(const QJsonObject& check, const QString& dir, QString* detail,
                      bool* skipped);
};

}  // namespace odv
