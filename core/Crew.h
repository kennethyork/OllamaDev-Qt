#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

#include "Backend.h"
#include "Sandbox.h"

namespace odv {

// One unit of work the Director hands to a Coder.
struct Subtask {
    int n = 0;
    QString title;
    QString role;     // persona from the role catalog
    QString prompt;
    QString backend;  // "ollama" | "claude" | "codex" | ...
    QString model;
    QString state;    // todo | doing | done | held | flagged
    QString route;    // the tier the router chose (simple|moderate|hard), or empty
};

struct AuditResult {
    bool clean = false;
    QString summary;
    QStringList issues;
};

// What one coder produced: a real changeset on disk, plus its audit.
struct CoderResult {
    int n = 0;
    bool empty = true;
    QString store;        // durable changeset dir
    QStringList files;
    QString diff;
    AuditResult audit;
    QString error;
};

struct CrewOptions {
    QString task;
    QString focus;
    int maxCoders = 4;
    bool research = true;
    bool audit = true;
    QString land = "auto";  // auto | review

    // Per-role routing. Empty means "inherit the session default".
    QString directorBackend, directorModel;
    QString researcherBackend, researcherModel;
    QString auditorBackend, auditorModel;
    QString coderBackend, coderModel;
    QStringList coderBackends;  // round-robin, one per coder — mix providers
    QStringList coderModels;

    // 0 = auto (derive from each backend's real concurrency limit).
    int parallel = 0;

    // The routing "brain": when on, any role with no explicitly pinned model gets
    // one auto-picked by difficulty — the Director and Auditor reason hard, the
    // Researcher moderate, and each coder is routed on ITS OWN subtask (difficulty
    // is a property of the subtask, not the role). Only applies to Ollama roles; a
    // Claude/Codex coder already brings its own model. An explicit --*-model always
    // wins over the router.
    bool route = false;
};

// Progress events, emitted from worker threads. The CLI prints them; the GUI
// marshals them onto the UI thread. Never blocks the pipeline.
struct CrewEvents {
    std::function<void(const QString& phase, const QString& msg)> onPhase;
    std::function<void(int coder, const QString& state)> onCoderState;
    std::function<void(int coder, const QString& chunk)> onCoderOutput;
    std::function<void(const QString& msg)> onLog;
};

// The bench: Researcher -> Director -> N Coders (parallel) -> Auditors
// (parallel) -> gated landing.
//
// Every coder builds inside its own copied sandbox and produces a changeset;
// landing copies the files into the project. No git anywhere.
class Crew {
public:
    struct Result {
        QString runId;
        QVector<Subtask> subtasks;
        QVector<CoderResult> results;
        QVector<int> applied;
        QVector<int> held;
    };

    static Result run(const CrewOptions& opts, const CrewEvents& ev, const CancelToken& cancel);

    // Held work, decided later by a human from the board.
    static bool accept(int n, QString* err = nullptr);
    static bool discard(int n, QString* err = nullptr);

    // Live steering: appended to steer.jsonl, injected between coder iterations.
    static bool steer(int coder, const QString& message, QString* err = nullptr);

    static QJsonObject boardState();  // ~/.ollamadev/crew/current.json
    static void clearBoard();
};

}  // namespace odv
