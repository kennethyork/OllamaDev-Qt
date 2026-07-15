// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

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

    // --- MDASH-style options, all OPT-IN. With none of these set, the crew runs
    // exactly as before. ---

    // Replace the single Auditor verdict with a 3-agent debate (advocate vs
    // skeptic vs judge) per changeset — a contested change survives an argument
    // before it lands.
    bool debate = false;

    // Detect coders whose work DUPLICATES another's (same feature in different
    // files) and hold the redundant ones, beyond the always-on path-overlap guard.
    bool dedupe = false;

    // Security-scan mode: the crew does not write code — it hunts vulnerabilities
    // (read-only) and produces a findings report.
    bool security = false;

    // Raise the coder cap for a bigger swarm (0 → the default cap of 8).
    int swarmMax = 0;

    // The learning loop: before the run, load what past runs learned (memory +
    // skills) into the crew's context; after the run, distill what THIS run
    // taught into durable memory (and a reusable skill when there's a pattern).
    // Off by default — the plain crew neither reads nor writes this.
    bool learn = false;

    // Self-consistency. 1 = one shot, exactly as before. With N > 1 the Director
    // draws N INDEPENDENT plans and keeps the one whose subtask count is the mode
    // — a weak model's planning variance averages out instead of deciding the run
    // — and the Auditor becomes an N-reviewer panel (alternating neutral and
    // skeptic) that only calls a changeset clean on a STRICT majority.
    //
    // It costs what it says: N× the Director's calls and N× each audit. The coders
    // are untouched, so the extra spend is on thinking, not on typing.
    int amplify = 1;

    // Resume an interrupted run: keep coders that already finished (landed from
    // disk, or held on the board) untouched and finish the rest. Empty = a fresh
    // run. When set, the plan on disk — not the other fields here — is the source
    // of truth for the task, focus, and per-coder prompts/models.
    //
    // By default resume brings the Director back to RE-PLAN the leftover work
    // (the finished coders are told to it as already-done, so it never re-plans
    // them). Set `replay` to skip the Director and literally re-run the saved
    // plan's unfinished subtasks instead.
    QString resumeRunId;
    bool replay = false;
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

    // Security-scan mode (opts.security). Read-only vulnerability hunt over the
    // project; writes a findings report and returns its runId. run() delegates
    // here when opts.security is set, so the regular crew is untouched.
    static Result securityScan(const CrewOptions& opts, const CrewEvents& ev,
                               const CancelToken& cancel);

    // Held work, decided later by a human from the board.
    static bool accept(int n, QString* err = nullptr);
    static bool discard(int n, QString* err = nullptr);

    // Live steering: appended to steer.jsonl, injected between coder iterations.
    static bool steer(int coder, const QString& message, QString* err = nullptr);

    static QJsonObject boardState();  // ~/.ollamadev/crew/current.json
    static void clearBoard();

    // Tail one coder's live log from `offset`. Returns the new bytes and sets
    // `size` to the log's current length, which is the offset for the next call —
    // a desktop pane polls this a few times a second and only ever ships the delta.
    //
    // The log carries the coder's raw output PLUS a line per tool call ("→ edit
    // src/Parser.cpp"), which is what lets a watcher see what it is DOING.
    static QString coderLog(const QString& runId, int coder, qint64 offset, qint64* size = nullptr);

    // One resumable run: a run dir that still has a plan.json. Newest first.
    struct RunInfo {
        QString runId;
        QString task;
        QString cwd;  // the project the run belongs to (empty for pre-cwd runs)
        int done = 0;
        int total = 0;
    };
    static QVector<RunInfo> resumable();
};

}  // namespace odv
