// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Puller.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopedPointer>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <unistd.h>

#include "Config.h"
#include "OllamaBackend.h"

namespace odv {
namespace {

constexpr int kMaxAttempts = 4;

// A pull is aborted by Ctrl-C, not killed: the handler only sets a flag, a timer
// turns that into reply->abort() on the request thread. Static because a POSIX
// signal handler has no user-data channel; guarded so only one pull arms it at a
// time (the CLI runs them one at a time anyway).
std::atomic_bool g_abort{false};
void onSigint(int) { g_abort.store(true); }

bool isTty() { return ::isatty(STDOUT_FILENO) != 0; }

// Same verdict/transient split as OllamaBackend::isTransient: retry a dropped
// connection or a briefly-dead/overloaded server, never a clean 4xx.
bool isTransient(int netError, int status) {
    if (status == 400 || status == 401 || status == 403 || status == 404) return false;
    if (status == 500 || status == 502 || status == 503 || status == 504 || status == 429)
        return true;
    if (status != 0) return false;
    switch (netError) {
        case QNetworkReply::ConnectionRefusedError:
        case QNetworkReply::RemoteHostClosedError:
        case QNetworkReply::HostNotFoundError:
        case QNetworkReply::TimeoutError:
        case QNetworkReply::TemporaryNetworkFailureError:
        case QNetworkReply::NetworkSessionFailedError:
        case QNetworkReply::ProxyConnectionClosedError:
        case QNetworkReply::ProxyTimeoutError:
        case QNetworkReply::UnknownNetworkError:
            return true;
        default:
            return false;
    }
}

void emitRaw(const QString& s) {
    const QByteArray b = s.toUtf8();
    std::fwrite(b.constData(), 1, size_t(b.size()), stdout);
    std::fflush(stdout);
}

}  // namespace

QString Puller::bytes(qint64 b) {
    if (b >= 1073741824) return QString::number(b / 1073741824.0, 'f', 1) + QStringLiteral(" GB");
    if (b >= 1048576) return QString::number(b / 1048576.0, 'f', 1) + QStringLiteral(" MB");
    if (b >= 1024) return QString::number(b / 1024.0, 'f', 1) + QStringLiteral(" KB");
    return QString::number(b) + QStringLiteral(" B");
}

bool Puller::pull(const QString& modelArg, const QString& hostArg, QString* err) {
    const QString model = modelArg.trimmed();
    if (model.isEmpty()) {
        if (err) *err = QStringLiteral("no model given");
        return false;
    }
    const QString host =
        hostArg.isEmpty() ? Config::str("ollama.host", "http://localhost:11434") : hostArg;
    const bool tty = isTty();

    const QByteArray payload =
        QJsonDocument(QJsonObject{{"model", model}, {"stream", true}}).toJson(QJsonDocument::Compact);

    // Arm Ctrl-C for the whole pull; the previous disposition is restored on every
    // return path so we never leave the process with our handler installed.
    g_abort.store(false);
    struct sigaction sa{}, prev{};
    sa.sa_handler = onSigint;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, &prev);
    struct Restore {
        struct sigaction* prev;
        ~Restore() { ::sigaction(SIGINT, prev, nullptr); }
    } restore{&prev};

    QString lastDrawn;
    const auto render = [&](const QString& line) {
        if (tty) {
            emitRaw(QStringLiteral("\r\033[K\033[2m  ") + line + QStringLiteral("\033[0m"));
        } else {
            if (line == lastDrawn) return;  // don't spam a logfile with identical lines
            emitRaw(QStringLiteral("  ") + line + QStringLiteral("\n"));
        }
        lastDrawn = line;
    };

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        QNetworkAccessManager nam;
        QNetworkRequest req{QUrl(host + QStringLiteral("/api/pull"))};
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        OllamaBackend::applyAuth(req);

        QEventLoop loop;
        QScopedPointer<QNetworkReply> reply(nam.post(req, payload));

        QByteArray buf;
        bool ok = false;
        QString serverError;
        int lastPct = -1;

        const auto onChunk = [&](const QByteArray& data) {
            buf.append(data);
            int nl;
            while ((nl = buf.indexOf('\n')) >= 0) {
                const QByteArray line = buf.left(nl).trimmed();
                buf.remove(0, nl + 1);
                if (line.isEmpty()) continue;
                const QJsonObject j = QJsonDocument::fromJson(line).object();
                if (j.isEmpty()) continue;
                const QString e = j.value("error").toString();
                if (!e.isEmpty()) {
                    serverError = e;  // a verdict; keep draining but do not retry
                    continue;
                }
                const QString status = j.value("status").toString();
                if (status.contains(QStringLiteral("success"), Qt::CaseInsensitive)) {
                    ok = true;
                    continue;
                }
                const qint64 total = qint64(j.value("total").toDouble());
                const qint64 done = qint64(j.value("completed").toDouble());
                if (total > 0) {
                    const int pct = int(done * 100 / total);
                    if (pct != lastPct) {
                        lastPct = pct;
                        render(QStringLiteral("pulling %1  %2%  (%3 / %4)")
                                   .arg(model)
                                   .arg(pct, 3)
                                   .arg(bytes(done), bytes(total)));
                    }
                } else {
                    render(QStringLiteral("pulling %1  %2")
                               .arg(model, status.isEmpty() ? QStringLiteral("...") : status));
                }
            }
        };

        QObject::connect(reply.data(), &QNetworkReply::readyRead, &loop,
                         [&]() { onChunk(reply->readAll()); });
        QObject::connect(reply.data(), &QNetworkReply::finished, &loop, &QEventLoop::quit);

        // Poll the Ctrl-C flag: CancelToken/atomic has no signal, and abort() must
        // run on this thread. We own `reply`, so we abort only our own request.
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
            if (g_abort.load() && reply->isRunning()) reply->abort();
        });
        poll.start(100);

        loop.exec();
        poll.stop();

        if (reply->isOpen()) onChunk(reply->readAll());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const int netError = static_cast<int>(reply->error());

        if (tty) emitRaw(QStringLiteral("\r\033[K"));

        if (ok) {
            emitRaw(QStringLiteral("\033[32m  ✓ pulled ") + model + QStringLiteral("\033[0m\n"));
            return true;
        }
        if (g_abort.load()) {
            emitRaw(QStringLiteral("\033[33m  pull cancelled\033[0m\n"));
            if (err) *err = QStringLiteral("cancelled");
            return false;
        }
        if (!serverError.isEmpty()) {  // the server said no — retrying cannot help
            emitRaw(QStringLiteral("\033[33m  pull failed: ") + serverError +
                    QStringLiteral("\033[0m\n"));
            if (err) *err = serverError;
            return false;
        }
        if (attempt < kMaxAttempts && isTransient(netError, status)) {
            emitRaw(QStringLiteral("\033[2m  network blip — resuming pull (attempt %1/%2)…\033[0m\n")
                        .arg(attempt + 1)
                        .arg(kMaxAttempts));
            // Short slices so a Ctrl-C during the wait is noticed at once.
            for (int slept = 0; slept < 800 && !g_abort.load(); slept += 50)
                QThread::msleep(50);
            if (g_abort.load()) {
                emitRaw(QStringLiteral("\033[33m  pull cancelled\033[0m\n"));
                if (err) *err = QStringLiteral("cancelled");
                return false;
            }
            continue;
        }
        const QString msg = reply->error() != QNetworkReply::NoError
                                ? reply->errorString()
                                : (status != 0 && status != 200 ? QStringLiteral("HTTP %1").arg(status)
                                                                : QStringLiteral("pull failed"));
        emitRaw(QStringLiteral("\033[33m  pull failed: ") + msg + QStringLiteral("\033[0m\n"));
        if (err) *err = msg;
        return false;
    }
    return false;
}

}  // namespace odv
