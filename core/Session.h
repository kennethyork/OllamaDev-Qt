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
