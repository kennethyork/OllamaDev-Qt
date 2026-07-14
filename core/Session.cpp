// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Session.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSaveFile>

#include <algorithm>

#include "Config.h"

namespace odv {
namespace {

QString sessionsDir() {
    return Config::dataDir() + QStringLiteral("/sessions");
}

QString pathFor(const QString& id) {
    return sessionsDir() + QLatin1Char('/') + id + QStringLiteral(".json");
}

void ensureDir() {
    QDir().mkpath(sessionsDir());
}

QString newId() {
    return QStringLiteral("session_%1_%2")
        .arg(QDateTime::currentSecsSinceEpoch())
        .arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0'));
}

QString canonical(const QString& path) {
    const QString c = QFileInfo(path).canonicalFilePath();
    return c.isEmpty() ? path : c;
}

QJsonObject encodeMessage(const ChatMessage& m) {
    QJsonObject o{{"role", m.role}, {"content", m.content}};
    if (!m.thinking.isEmpty()) o.insert("thinking", m.thinking);
    if (!m.toolCalls.isEmpty()) o.insert("tool_calls", m.toolCalls);
    if (!m.toolCallId.isEmpty()) o.insert("tool_call_id", m.toolCallId);
    if (!m.toolName.isEmpty()) o.insert("tool_name", m.toolName);
    if (!m.images.isEmpty()) {
        QJsonArray imgs;
        for (const QString& b64 : m.images) imgs.append(b64);
        o.insert("images", imgs);
    }
    return o;
}

ChatMessage decodeMessage(const QJsonObject& o) {
    ChatMessage m;
    m.role = o.value(QStringLiteral("role")).toString();
    m.content = o.value(QStringLiteral("content")).toString();
    m.thinking = o.value(QStringLiteral("thinking")).toString();
    m.toolCalls = o.value(QStringLiteral("tool_calls")).toArray();
    m.toolCallId = o.value(QStringLiteral("tool_call_id")).toString();
    m.toolName = o.value(QStringLiteral("tool_name")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("images")).toArray())
        m.images << v.toString();
    return m;
}

QJsonObject readFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray raw = f.readAll();
    f.close();
    return QJsonDocument::fromJson(raw).object();
}

// The first user message is a far better label for a session than "Session
// 2026-07-13 10:04", which is what every session would otherwise be called.
QString titleFrom(const QVector<ChatMessage>& msgs) {
    for (const ChatMessage& m : msgs) {
        if (m.role != QLatin1String("user")) continue;
        QString t = m.content.simplified();
        if (t.isEmpty()) continue;
        if (t.size() > 60) t = t.left(57) + QStringLiteral("…");
        return t;
    }
    return {};
}

}  // namespace

Session Session::create(const QString& cwd) {
    Session s;
    s.id_ = newId();
    s.cwd_ = cwd.isEmpty() ? QDir::currentPath() : cwd;
    s.created_ = QDateTime::currentSecsSinceEpoch();
    s.updated_ = s.created_;
    s.title_ = QStringLiteral("Session ") +
               QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    return s;
}

std::optional<Session> Session::load(const QString& id) {
    const QJsonObject o = readFile(pathFor(id));
    if (o.isEmpty()) return std::nullopt;

    Session s;
    s.id_ = o.value(QStringLiteral("id")).toString(id);
    s.title_ = o.value(QStringLiteral("title")).toString();
    s.cwd_ = o.value(QStringLiteral("cwd")).toString();
    s.model_ = o.value(QStringLiteral("model")).toString();
    s.backend_ = o.value(QStringLiteral("backend")).toString();
    s.created_ = qint64(o.value(QStringLiteral("created")).toDouble());
    s.updated_ = qint64(o.value(QStringLiteral("updated")).toDouble());
    for (const QJsonValue& v : o.value(QStringLiteral("messages")).toArray())
        s.messages_.append(decodeMessage(v.toObject()));
    return s;
}

std::optional<Session> Session::latestForCwd(const QString& cwd) {
    const QString target = canonical(cwd.isEmpty() ? QDir::currentPath() : cwd);
    for (const SessionMeta& m : list()) {  // already newest-first
        if (m.messages < 1 || m.cwd.isEmpty()) continue;
        if (canonical(m.cwd) != target) continue;
        return load(m.id);
    }
    return std::nullopt;
}

QVector<SessionMeta> Session::list() {
    QVector<SessionMeta> out;
    QDir dir(sessionsDir());
    if (!dir.exists()) return out;

    const QStringList files =
        dir.entryList({QStringLiteral("session_*.json")}, QDir::Files, QDir::NoSort);
    for (const QString& f : files) {
        const QJsonObject o = readFile(dir.filePath(f));
        if (o.isEmpty()) continue;  // a half-written or hand-mangled file is skipped, not fatal
        SessionMeta m;
        m.id = o.value(QStringLiteral("id")).toString(QFileInfo(f).completeBaseName());
        m.title = o.value(QStringLiteral("title")).toString();
        m.cwd = o.value(QStringLiteral("cwd")).toString();
        m.model = o.value(QStringLiteral("model")).toString();
        m.backend = o.value(QStringLiteral("backend")).toString();
        m.updated = qint64(o.value(QStringLiteral("updated")).toDouble());
        m.messages = o.value(QStringLiteral("messages")).toArray().size();
        out.append(m);
    }
    std::sort(out.begin(), out.end(),
              [](const SessionMeta& a, const SessionMeta& b) { return a.updated > b.updated; });
    return out;
}

bool Session::remove(const QString& id) {
    return QFile::remove(pathFor(id));
}

QJsonObject Session::exportOne(const QString& id) {
    QFile f(pathFor(id));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

QString Session::importOne(const QJsonObject& o, const QString& cwd, QString* err) {
    const QJsonArray msgs = o.value(QStringLiteral("messages")).toArray();
    if (msgs.isEmpty()) {
        if (err) *err = QStringLiteral("no messages in it");
        return {};
    }

    Session s = Session::create(cwd);
    // Decode and re-encode through the SAME codec the app uses, so tool calls,
    // tool ids, thinking and attached images all survive. (The PHP import replayed
    // only role+content, which quietly destroyed the tool-call correlation.)
    for (const QJsonValue& v : msgs)
        if (v.isObject()) s.messages_.append(decodeMessage(v.toObject()));

    const QString model = o.value(QStringLiteral("model")).toString();
    const QString backend = o.value(QStringLiteral("backend")).toString();
    if (!model.isEmpty()) s.model_ = model;
    if (!backend.isEmpty()) s.backend_ = backend;
    s.save();

    // save() derives a title from the first user message, so an imported session
    // is titled the same way a fresh one is — but an explicit exported title is
    // better than a derived one, so it wins.
    const QString title = o.value(QStringLiteral("title")).toString();
    if (!title.isEmpty()) {
        s.title_ = title;
        QJsonObject raw = exportOne(s.id_);
        raw.insert(QStringLiteral("title"), title);
        QSaveFile f(pathFor(s.id_));
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(raw).toJson(QJsonDocument::Indented));
            f.commit();
        }
    }
    return s.id_;
}

void Session::save() {
    ensureDir();
    updated_ = QDateTime::currentSecsSinceEpoch();

    const QString auto_ = titleFrom(messages_);
    if (!auto_.isEmpty()) title_ = auto_;

    QJsonArray msgs;
    for (const ChatMessage& m : messages_) msgs.append(encodeMessage(m));

    QJsonObject o{{"id", id_},
                  {"title", title_},
                  {"cwd", cwd_},
                  {"model", model_},
                  {"backend", backend_},
                  {"created", double(created_)},
                  {"updated", double(updated_)},
                  {"messages", msgs}};

    // QSaveFile writes a temp file and renames it into place: an interrupted save
    // can lose the NEW state but never corrupts the old file, which is the one the
    // next `-c` has to be able to read.
    QSaveFile f(pathFor(id_));
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    f.commit();
}

void Session::compact(int keepRecent) {
    const int keep = qMax(2, keepRecent);
    if (messages_.size() <= keep + 1) return;

    // messages_[0] is the agent's system prompt, and Agent::ensureSystem OVERWRITES
    // the content of a leading system message on every turn. Parking the summary
    // there would therefore delete it on the very next request — so the head stays
    // put and the summary is inserted after it.
    const int head = (!messages_.isEmpty() && messages_.first().role == QLatin1String("system"))
                         ? 1 : 0;

    int cut = messages_.size() - keep;
    if (cut <= head + 1) return;  // nothing worth summarising

    // A tool result whose assistant tool_calls message got summarised away is an
    // orphan the model (and Ollama) cannot make sense of, so the tail is walked
    // forward to the first message that can legally start a conversation.
    while (cut < messages_.size() && messages_.at(cut).role == QLatin1String("tool")) ++cut;
    if (cut >= messages_.size()) return;

    QString transcript;
    for (int i = head; i < cut; ++i) {
        const ChatMessage& m = messages_.at(i);
        const QString body = m.content.trimmed();
        if (body.isEmpty()) continue;
        QString label = m.role.toUpper();
        if (!m.toolName.isEmpty()) label += QStringLiteral("(%1)").arg(m.toolName);
        transcript += label + QStringLiteral(": ") + body.left(4000) + QLatin1Char('\n');
    }

    QString summary;
    if (const BackendPtr be = Backends::get(backend_.isEmpty() ? QStringLiteral("ollama")
                                                               : backend_)) {
        QVector<ChatMessage> ask{
            {QStringLiteral("system"),
             QStringLiteral("Summarise this coding-session transcript for a model that must "
                            "carry on the work. Keep: the user's goals, decisions taken, files "
                            "touched and their state, and anything still open. Drop pleasantries. "
                            "Reply with the summary only."),
             {}, {}, {}, {}, {}},
            {QStringLiteral("user"), transcript, {}, {}, {}, {}, {}}};
        StreamSink quiet;
        CancelToken cancel;
        const ChatTurn t = be->chat(model_, ask, QJsonArray(), quiet, cancel);
        if (t.ok) summary = t.content.trimmed();
    }

    if (summary.isEmpty()) {
        // Backend down: still bound the growth, just with a dumber summary.
        for (int i = head; i < cut; ++i) {
            const ChatMessage& m = messages_.at(i);
            const QString body = m.content.trimmed();
            if (body.isEmpty()) continue;
            summary += QStringLiteral("- %1: %2…\n").arg(m.role.toUpper(), body.left(150));
        }
    }

    ChatMessage note;
    note.role = QStringLiteral("system");
    note.content = QStringLiteral("Summary of the earlier part of this conversation:\n") + summary;

    QVector<ChatMessage> next;
    if (head) next.append(messages_.first());
    next.append(note);
    for (int i = cut; i < messages_.size(); ++i) next.append(messages_.at(i));
    messages_ = next;
    save();
}

SessionMeta Session::meta() const {
    SessionMeta m;
    m.id = id_;
    m.title = title_;
    m.cwd = cwd_;
    m.model = model_;
    m.backend = backend_;
    m.updated = updated_;
    m.messages = messages_.size();
    return m;
}

}  // namespace odv
