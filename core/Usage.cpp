// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Usage.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>

#include "Config.h"

namespace odv {
namespace {

// Persisted under the per-project data dir so totals survive turns, resumes, and
// a separate `ollamadev stats` process (which is why record() writes to disk on
// every turn rather than only accumulating in memory).
QString usagePath() { return Config::dataDir() + QStringLiteral("/costs/usage.json"); }

QMutex& mutex() {
    static QMutex m;
    return m;
}

QJsonObject readUsage() {
    QFile f(usagePath());
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray raw = f.readAll();
    f.close();
    return QJsonDocument::fromJson(raw).object();
}

QString humanTokens(qint64 n) {
    if (n >= 1000000) return QStringLiteral("%1M").arg(double(n) / 1e6, 0, 'f', 2);
    if (n >= 1000) return QStringLiteral("%1k").arg(double(n) / 1e3, 0, 'f', 1);
    return QString::number(n);
}

}  // namespace

void Usage::record(const QString& model, int promptTokens, int evalTokens) {
    if (promptTokens <= 0 && evalTokens <= 0) return;  // nothing real to count

    QMutexLocker lock(&mutex());  // crew coders call turn() (and this) concurrently

    QJsonObject root = readUsage();
    root.insert(QStringLiteral("total_prompt"),
                root.value(QStringLiteral("total_prompt")).toInt() + promptTokens);
    root.insert(QStringLiteral("total_eval"),
                root.value(QStringLiteral("total_eval")).toInt() + evalTokens);
    root.insert(QStringLiteral("turns"), root.value(QStringLiteral("turns")).toInt() + 1);
    root.insert(QStringLiteral("updated_at"), QDateTime::currentDateTime().toString(Qt::ISODate));

    // Per-model breakdown, so `stats` can show where the tokens went in a session
    // that switched models mid-way.
    QJsonObject models = root.value(QStringLiteral("models")).toObject();
    const QString key = model.isEmpty() ? QStringLiteral("(unknown)") : model;
    QJsonObject m = models.value(key).toObject();
    m.insert(QStringLiteral("prompt"), m.value(QStringLiteral("prompt")).toInt() + promptTokens);
    m.insert(QStringLiteral("eval"), m.value(QStringLiteral("eval")).toInt() + evalTokens);
    m.insert(QStringLiteral("turns"), m.value(QStringLiteral("turns")).toInt() + 1);
    models.insert(key, m);
    root.insert(QStringLiteral("models"), models);

    const QString file = usagePath();
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile out(file);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        out.commit();
    }
}

QMap<QString, Usage::Tally> Usage::snapshot() {
    QMutexLocker lock(&mutex());
    QMap<QString, Tally> out;
    const QJsonObject models = readUsage().value(QStringLiteral("models")).toObject();
    for (auto it = models.constBegin(); it != models.constEnd(); ++it) {
        const QJsonObject m = it.value().toObject();
        out.insert(it.key(),
                   Tally{m.value(QStringLiteral("prompt")).toInt(),
                         m.value(QStringLiteral("eval")).toInt()});
    }
    return out;
}

QString Usage::report() {
    QMutexLocker lock(&mutex());
    const QJsonObject root = readUsage();

    const qint64 prompt = root.value(QStringLiteral("total_prompt")).toInt();
    const qint64 eval = root.value(QStringLiteral("total_eval")).toInt();
    const int turns = root.value(QStringLiteral("turns")).toInt();
    if (prompt <= 0 && eval <= 0) return QStringLiteral("no usage recorded yet\n");

    QString o = QStringLiteral("\nToken usage (this project)\n");
    o += QString(34, QChar(0x2500)) + QLatin1Char('\n');
    o += QStringLiteral("  turns        %1\n").arg(turns);
    o += QStringLiteral("  prompt       %1 tokens\n").arg(humanTokens(prompt));
    o += QStringLiteral("  generated    %1 tokens\n").arg(humanTokens(eval));
    o += QStringLiteral("  total        %1 tokens\n").arg(humanTokens(prompt + eval));

    const QJsonObject models = root.value(QStringLiteral("models")).toObject();
    if (models.size() > 1 || (models.size() == 1)) {
        o += QStringLiteral("\n  By model:\n");
        for (auto it = models.constBegin(); it != models.constEnd(); ++it) {
            const QJsonObject m = it.value().toObject();
            o += QStringLiteral("    %1  %2 prompt / %3 gen  (%4 turns)\n")
                     .arg(it.key(),
                          humanTokens(m.value(QStringLiteral("prompt")).toInt()),
                          humanTokens(m.value(QStringLiteral("eval")).toInt()))
                     .arg(m.value(QStringLiteral("turns")).toInt());
        }
    }
    const QString updated = root.value(QStringLiteral("updated_at")).toString();
    if (!updated.isEmpty()) o += QStringLiteral("\n  updated %1\n").arg(updated);
    return o;
}

}  // namespace odv
