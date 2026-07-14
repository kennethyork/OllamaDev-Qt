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
#include <QString>
#include <QVector>
#include <optional>

#include "Backend.h"

namespace odv {

struct SessionMeta {
    QString id;
    QString title;
    QString cwd;
    QString model;
    QString backend;
    qint64 updated = 0;  // unix seconds
    int messages = 0;
};

// A conversation, persisted as one JSON file under
// <Config::dataDir()>/sessions/<id>.json — i.e. PER PROJECT (.ollamadev/ in the
// repo), exactly like the PHP original. That is what makes `-c` mean "carry on
// where I left off IN THIS REPO" instead of resuming whatever conversation you
// last had anywhere on the machine.
//
// Persistence only. The chat loop lives in cli/Repl.cpp: a session that knew how
// to read a keystroke could not be reused by the GUI.
class Session {
public:
    static Session create(const QString& cwd);
    static std::optional<Session> load(const QString& id);

    // Newest session belonging to `cwd` that actually has messages — an empty
    // just-created session is not something you resume into.
    static std::optional<Session> latestForCwd(const QString& cwd);

    static QVector<SessionMeta> list();  // newest first
    static bool remove(const QString& id);

    // ---- portability -------------------------------------------------------
    // The session's own on-disk JSON, verbatim — which is the highest-fidelity
    // export there is, because it is exactly what the app reads back.
    //
    // The PHP export hand-rolled {id, messages, model} and so DROPPED tool_calls,
    // meaning a session with any tool use in it did not survive a round trip: the
    // assistant turns still said "I called edit()" and the correlation to the tool
    // results was gone. It also dropped the title, the cwd, and every timestamp.
    static QJsonObject exportOne(const QString& id);

    // Import always MINTS A NEW ID, so it can never clobber a session you have —
    // there is no collision to resolve, by construction. Everything else (messages
    // with their tool calls, model, backend, title) is preserved. Returns the new
    // id, or empty on failure.
    static QString importOne(const QJsonObject& o, const QString& cwd, QString* err = nullptr);

    QString id() const { return id_; }
    QVector<ChatMessage>& messages() { return messages_; }
    const QVector<ChatMessage>& messages() const { return messages_; }

    QString model() const { return model_; }
    void setModel(const QString& m) { model_ = m; }
    QString backend() const { return backend_; }
    void setBackend(const QString& b) { backend_ = b; }
    QString title() const { return title_; }
    QString cwd() const { return cwd_; }

    // Atomic: a crash mid-write leaves the previous session file intact rather
    // than a truncated one that would fail to parse on the next resume.
    void save();

    // Summarise everything except the last `keepRecent` messages into a single
    // system note, and drop the originals. The summary is asked of the session's
    // own model; if the backend is unreachable we fall back to a truncating join,
    // because the point of compaction is that the transcript stops growing —
    // that has to hold even when the model does not answer.
    void compact(int keepRecent);

    SessionMeta meta() const;

private:
    QString id_;
    QString title_;
    QString cwd_;
    QString model_;
    QString backend_;
    qint64 created_ = 0;
    qint64 updated_ = 0;
    QVector<ChatMessage> messages_;
};

}  // namespace odv
