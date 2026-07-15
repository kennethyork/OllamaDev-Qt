// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "InlineCompleter.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include "Config.h"
#include "OllamaBackend.h"

namespace odv {

InlineCompleter::InlineCompleter(QObject* parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this)) {}

bool InlineCompleter::enabled() {
    return Config::boolean(QStringLiteral("editor.autocomplete"), true);
}

QString InlineCompleter::model() {
    return Config::str(QStringLiteral("editor.autocomplete.model"),
                       QStringLiteral("qwen2.5-coder"));
}

void InlineCompleter::cancel() {
    if (reply_) {
        QNetworkReply* r = reply_;
        reply_ = nullptr;      // clear first so the finished handler ignores it
        r->abort();
        r->deleteLater();
    }
}

void InlineCompleter::request(const QString& prefix, const QString& suffix) {
    cancel();
    if (!enabled() || prefix.isEmpty()) {
        emit suggestion(QString());
        return;
    }

    const QString host =
        Config::str(QStringLiteral("ollama.host"), QStringLiteral("http://localhost:11434"));
    QNetworkRequest req{QUrl(host + QStringLiteral("/api/generate"))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    OllamaBackend::applyAuth(req);
    req.setTransferTimeout(4000);  // a suggestion that arrives after you've typed on is useless

    // A tight, low-temperature generation: we want the single most likely
    // continuation, not creativity, and we cap it so a runaway model can't stall
    // the ghost. keep_alive holds the model resident between keystrokes.
    const QJsonObject body{
        {QStringLiteral("model"), model()},
        {QStringLiteral("prompt"), prefix},
        {QStringLiteral("suffix"), suffix},
        {QStringLiteral("stream"), false},
        {QStringLiteral("keep_alive"), QStringLiteral("10m")},
        {QStringLiteral("options"),
         QJsonObject{{QStringLiteral("temperature"), 0.1},
                     {QStringLiteral("num_predict"), 96},
                     {QStringLiteral("top_p"), 0.9}}},
    };

    reply_ = nam_->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QNetworkReply* r = reply_;
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        if (reply_ != r) return;  // superseded by a newer request (or cancelled)
        reply_ = nullptr;
        if (r->error() != QNetworkReply::NoError) {
            emit suggestion(QString());  // model missing, offline, timeout → no ghost
            return;
        }
        const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
        emit suggestion(o.value(QStringLiteral("response")).toString());
    });
}

}  // namespace odv
