// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Board.h"

#include "Config.h"

#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QThread>

#include <algorithm>

namespace odv {
namespace {

constexpr int kPollMs = 250;       // responsive without busy-looping
constexpr int kRecentKept = 20;    // how many decided records current.json carries
constexpr int kLockWaitMs = 5000;  // a writer holds the lock for one append + one rewrite

QString boardDir() { return Config::boardDir(); }
QString lockPath() { return boardDir() + QStringLiteral("/.board.lock"); }

// Every mutation (append + index rewrite) happens under this, so the CLI and the
// GUI can post/decide concurrently without interleaving a half-written line.
// Not reentrant: never take it twice in one call path.
class BoardLock {
public:
    BoardLock() : lock_(lockPath()) {
        QDir().mkpath(boardDir());
        lock_.setStaleLockTime(30000);  // a crashed writer must not wedge the board
        ok_ = lock_.tryLock(kLockWaitMs);
    }
    bool ok() const { return ok_; }

private:
    QLockFile lock_;
    bool ok_ = false;
};

QJsonObject toJson(const Decision& d) {
    QJsonObject o;
    o.insert(QStringLiteral("id"), d.id);
    o.insert(QStringLiteral("kind"), d.kind);
    o.insert(QStringLiteral("summary"), d.summary);
    o.insert(QStringLiteral("detail"), d.detail);
    o.insert(QStringLiteral("data"), d.data);
    o.insert(QStringLiteral("ts"), d.ts);
    o.insert(QStringLiteral("verdict"), d.verdict);
    return o;
}

Decision fromJson(const QJsonObject& o) {
    Decision d;
    d.id = o.value(QStringLiteral("id")).toString();
    d.kind = o.value(QStringLiteral("kind")).toString();
    d.summary = o.value(QStringLiteral("summary")).toString();
    d.detail = o.value(QStringLiteral("detail")).toString();
    d.data = o.value(QStringLiteral("data")).toObject();
    d.ts = qint64(o.value(QStringLiteral("ts")).toDouble());
    d.verdict = o.value(QStringLiteral("verdict")).toString();
    return d;
}

// Replay the append-only log. Later "decide" events fold into the record they
// name, so replaying is what turns the audit trail back into current state.
QVector<Decision> replayLog() {
    QVector<Decision> out;
    QFile f(Board::logFile());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QHash<QString, int> at;  // id -> index in `out`, preserving arrival order
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (line.isEmpty()) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) continue;  // a torn line never corrupts the replay
        const QJsonObject o = doc.object();
        const QString id = o.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) continue;
        if (o.value(QStringLiteral("event")).toString() == QLatin1String("decide")) {
            const QString v = o.value(QStringLiteral("verdict")).toString();
            const auto it = at.constFind(id);
            if (it != at.constEnd()) {
                out[it.value()].verdict = v.isEmpty() ? QStringLiteral("deny") : v;
            } else {
                // Verdict for a record we never saw (log truncated). Keep it in
                // the trail rather than dropping the audit entry.
                Decision d;
                d.id = id;
                d.kind = QStringLiteral("?");
                d.summary = QStringLiteral("(unknown)");
                d.ts = qint64(o.value(QStringLiteral("ts")).toDouble());
                d.verdict = v.isEmpty() ? QStringLiteral("deny") : v;
                at.insert(id, int(out.size()));
                out.append(d);
            }
            continue;
        }
        Decision d = fromJson(o);
        const auto it = at.constFind(id);
        if (it != at.constEnd()) out[it.value()] = d;
        else {
            at.insert(id, int(out.size()));
            out.append(d);
        }
    }
    return out;
}

bool appendLine(const QJsonObject& o) {
    QDir().mkpath(boardDir());
    QFile f(Board::logFile());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) return false;
    const QByteArray line = QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
    return f.write(line) == line.size();
}

// QSaveFile is temp-file + atomic rename: a poller reading current.json while we
// rewrite it sees either the old file or the new one, never a truncated middle.
bool atomicWrite(const QString& path, const QByteArray& data) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    if (f.write(data) != data.size()) return false;
    return f.commit();
}

QJsonArray toArray(const QVector<Decision>& v) {
    QJsonArray a;
    for (const Decision& d : v) a.append(toJson(d));
    return a;
}

// Caller must already hold the BoardLock.
void rebuildIndex(const QVector<Decision>& all) {
    QVector<Decision> pending;
    QVector<Decision> recent;
    for (const Decision& d : all) {
        if (d.verdict.isEmpty()) pending.append(d);
        else recent.prepend(d);  // newest first
    }
    std::stable_sort(pending.begin(), pending.end(),
                     [](const Decision& a, const Decision& b) { return a.ts < b.ts; });
    if (recent.size() > kRecentKept) recent.resize(kRecentKept);

    QJsonObject idx;
    idx.insert(QStringLiteral("pending"), toArray(pending));
    idx.insert(QStringLiteral("recent"), toArray(recent));
    idx.insert(QStringLiteral("ts"), QDateTime::currentSecsSinceEpoch());
    QDir().mkpath(boardDir());
    atomicWrite(Board::indexFile(), QJsonDocument(idx).toJson(QJsonDocument::Compact));
}

QString newId(const QString& kind) {
    const QString prefix = (kind.isEmpty() ? QStringLiteral("dec") : kind.left(4));
    return QStringLiteral("%1_%2_%3")
        .arg(prefix)
        .arg(QDateTime::currentMSecsSinceEpoch(), 8, 16, QLatin1Char('0'))
        .arg(QRandomGenerator::global()->generate(), 6, 16, QLatin1Char('0'))
        .left(32);
}

// Read the derived index; fall back to replaying the log when it is missing
// (first run, or someone deleted it).
QVector<Decision> readIndexSection(const QString& section) {
    QFile f(Board::indexFile());
    if (!f.open(QIODevice::ReadOnly)) {
        QVector<Decision> out;
        for (const Decision& d : replayLog()) {
            const bool want = (section == QLatin1String("pending")) ? d.verdict.isEmpty()
                                                                    : !d.verdict.isEmpty();
            if (!want) continue;
            if (section == QLatin1String("pending")) out.append(d);
            else out.prepend(d);
        }
        if (section == QLatin1String("pending"))
            std::stable_sort(out.begin(), out.end(),
                             [](const Decision& a, const Decision& b) { return a.ts < b.ts; });
        return out;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    QVector<Decision> out;
    for (const QJsonValue& v : doc.object().value(section).toArray())
        if (v.isObject()) out.append(fromJson(v.toObject()));
    return out;
}

}  // namespace

QStringList Board::verdicts() {
    return {QStringLiteral("accept"), QStringLiteral("deny"), QStringLiteral("always"),
            QStringLiteral("skip")};
}

QString Board::logFile() { return boardDir() + QStringLiteral("/decisions.jsonl"); }
QString Board::indexFile() { return boardDir() + QStringLiteral("/current.json"); }

QString Board::enqueue(const Decision& d) {
    Decision rec = d;
    if (rec.kind.isEmpty()) rec.kind = QStringLiteral("permission");
    if (rec.id.isEmpty()) rec.id = newId(rec.kind);
    if (rec.ts == 0) rec.ts = QDateTime::currentSecsSinceEpoch();
    rec.verdict.clear();  // enqueue always posts a pending record

    BoardLock lock;
    if (!lock.ok()) return QString();
    QJsonObject o = toJson(rec);
    o.insert(QStringLiteral("event"), QStringLiteral("enqueue"));
    if (!appendLine(o)) return QString();
    rebuildIndex(replayLog());
    return rec.id;
}

QVector<Decision> Board::pending() { return readIndexSection(QStringLiteral("pending")); }

QVector<Decision> Board::recent(int limit) {
    QVector<Decision> r = readIndexSection(QStringLiteral("recent"));
    if (limit >= 0 && r.size() > limit) r.resize(limit);
    return r;
}

bool Board::decide(const QString& id, const QString& verdict, QString* err) {
    const QString v = verdict.trimmed().toLower();
    if (!verdicts().contains(v)) {
        if (err) *err = QStringLiteral("unknown verdict: ") + verdict;
        return false;
    }
    BoardLock lock;
    if (!lock.ok()) {
        if (err) *err = QStringLiteral("board is busy (could not take the lock)");
        return false;
    }
    QVector<Decision> all = replayLog();
    const auto it = std::find_if(all.begin(), all.end(),
                                 [&id](const Decision& d) { return d.id == id; });
    if (it == all.end()) {
        if (err) *err = QStringLiteral("no such decision: ") + id;
        return false;
    }
    if (!it->verdict.isEmpty()) {  // idempotent: first verdict wins
        if (err) *err = QStringLiteral("already decided: ") + it->verdict;
        return false;
    }

    QJsonObject ev;
    ev.insert(QStringLiteral("id"), id);
    ev.insert(QStringLiteral("event"), QStringLiteral("decide"));
    ev.insert(QStringLiteral("verdict"), v);
    ev.insert(QStringLiteral("ts"), QDateTime::currentSecsSinceEpoch());
    if (!appendLine(ev)) {
        if (err) *err = QStringLiteral("could not write the decision log");
        return false;
    }
    it->verdict = v;
    rebuildIndex(all);
    return true;
}

std::optional<Decision> Board::get(const QString& id) {
    for (const Decision& d : replayLog())
        if (d.id == id) return d;
    return std::nullopt;
}

void Board::clear() {
    BoardLock lock;
    if (!lock.ok()) return;
    QFile::remove(logFile());
    rebuildIndex({});
}

QString Board::waitFor(const QString& id, int timeoutSeconds) {
    const QDeadlineTimer deadline = timeoutSeconds > 0
                                        ? QDeadlineTimer(qint64(timeoutSeconds) * 1000)
                                        : QDeadlineTimer(QDeadlineTimer::Forever);
    while (true) {
        const std::optional<Decision> d = get(id);
        // The record is gone (board cleared, log rotated). Fail closed — this
        // gate is what stands between a model and the user's filesystem.
        if (!d) return QStringLiteral("deny");
        if (!d->verdict.isEmpty()) return d->verdict;
        if (deadline.hasExpired()) return QStringLiteral("timeout");
        QThread::msleep(kPollMs);
    }
}

}  // namespace odv
