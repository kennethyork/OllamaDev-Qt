// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Update.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSysInfo>
#include <QTemporaryFile>

#include "Version.h"

namespace odv {
namespace {

constexpr const char* kReleaseApi =
    "https://api.github.com/repos/kennethyork/OllamaDev-Qt/releases/latest";

QByteArray httpGet(const QUrl& url, QString* err, qint64* size = nullptr,
                   const std::function<void(qint64, qint64)>& progress = {}) {
    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("OllamaDev/%1").arg(QStringLiteral(ODV_VERSION)));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);  // release assets 302 to S3

    QScopedPointer<QNetworkReply> reply(nam.get(req));
    QEventLoop loop;
    QObject::connect(reply.data(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (progress)
        QObject::connect(reply.data(), &QNetworkReply::downloadProgress, &loop,
                         [&progress](qint64 got, qint64 total) { progress(got, total); });
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        if (err) *err = reply->errorString();
        return {};
    }
    const QByteArray body = reply->readAll();
    if (size) *size = body.size();
    return body;
}

// The asset name for THIS machine. If there is no such asset we fail — we do NOT
// fall back to whatever asset happens to be first, which is what the PHP did and
// which installs a binary that cannot run.
QString wantedAsset() {
    QString os = QStringLiteral("linux");
#if defined(Q_OS_WIN)
    os = QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    os = QStringLiteral("mac");
#endif
    const QString arch = QSysInfo::currentCpuArchitecture().contains(QStringLiteral("arm"))
                             ? QStringLiteral("arm64")
                             : QStringLiteral("x64");
    QString name = QStringLiteral("ollamadev-%1-%2").arg(os, arch);
#ifdef Q_OS_WIN
    name += QStringLiteral(".exe");
#endif
    return name;
}

// "v0.2.1" / "0.2.1" -> comparable. Returns true when `a` is strictly newer.
bool newerThan(const QString& a, const QString& b) {
    const auto parts = [](QString v) {
        if (v.startsWith(QLatin1Char('v'))) v.remove(0, 1);
        QVector<int> n;
        for (const QString& p : v.split(QLatin1Char('.'))) n << p.split(QLatin1Char('-')).first().toInt();
        while (n.size() < 3) n << 0;
        return n;
    };
    const QVector<int> x = parts(a), y = parts(b);
    for (int i = 0; i < 3; ++i) {
        if (x.at(i) != y.at(i)) return x.at(i) > y.at(i);
    }
    return false;
}

}  // namespace

UpdateInfo Update::check() {
    UpdateInfo u;
    u.current = QStringLiteral(ODV_VERSION);
    // The RESOLVED path of the running binary — never argv[0], which through a
    // symlink or a relative invocation names something else entirely.
    u.target = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();

    QString err;
    const QByteArray body = httpGet(QUrl(QLatin1String(kReleaseApi)), &err);
    if (body.isEmpty()) {
        u.error = err.isEmpty() ? QStringLiteral("could not reach GitHub") : err;
        return u;
    }
    const QJsonObject rel = QJsonDocument::fromJson(body).object();
    if (rel.isEmpty() || rel.contains(QStringLiteral("message"))) {
        // GitHub answers rate limiting with {"message": "API rate limit exceeded…"}.
        u.error = rel.value(QStringLiteral("message"))
                      .toString(QStringLiteral("GitHub returned nothing usable"));
        return u;
    }

    u.latest = rel.value(QStringLiteral("tag_name")).toString();
    if (u.latest.isEmpty()) {
        u.error = QStringLiteral("the latest release has no tag");
        return u;
    }
    u.notes = rel.value(QStringLiteral("body")).toString();
    u.newer = newerThan(u.latest, u.current);

    const QString want = wantedAsset();
    for (const QJsonValue& v : rel.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject a = v.toObject();
        if (a.value(QStringLiteral("name")).toString() != want) continue;
        u.assetName = want;
        u.assetUrl = a.value(QStringLiteral("browser_download_url")).toString();
        u.assetSize = static_cast<qint64>(a.value(QStringLiteral("size")).toDouble());
        break;
    }
    if (u.newer && u.assetUrl.isEmpty()) {
        // Deliberately an ERROR, not a fallback to some other asset: a binary for
        // another platform is not an update, it is a brick.
        u.error = QStringLiteral("release %1 has no asset for this machine (%2)")
                      .arg(u.latest, want);
        return u;
    }

    u.ok = true;
    return u;
}

bool Update::install(const UpdateInfo& info, QString* err,
                     const std::function<void(qint64, qint64)>& progress) {
    const auto fail = [&](const QString& m) {
        if (err) *err = m;
        return false;
    };
    if (info.assetUrl.isEmpty()) return fail(QStringLiteral("nothing to install"));
    if (info.target.isEmpty()) return fail(QStringLiteral("cannot tell which file I am"));

    const QFileInfo ti(info.target);
    const QString dir = ti.absolutePath();
    // Check we can write BEFORE downloading 40MB. A root-owned /usr/local/bin is
    // the common case and it should say "run me with sudo", not fail at the end.
    if (!QFileInfo(dir).isWritable())
        return fail(QStringLiteral("%1 is not writable — re-run with sudo, or install elsewhere")
                        .arg(dir));

    QString e;
    const QByteArray blob = httpGet(QUrl(info.assetUrl), &e, nullptr, progress);
    if (blob.isEmpty()) return fail(e.isEmpty() ? QStringLiteral("download failed") : e);
    // A short download is a truncated binary, and a truncated binary is a brick.
    if (info.assetSize > 0 && blob.size() != info.assetSize)
        return fail(QStringLiteral("download is %1 bytes, expected %2 — refusing to install it")
                        .arg(blob.size())
                        .arg(info.assetSize));

    // Write the new binary NEXT TO the target, never in /tmp: the swap has to be a
    // rename, and a rename across filesystems fails. (PHP's did, on any machine
    // where /tmp is a separate mount.)
    const QString fresh = info.target + QStringLiteral(".new");
    const QString backup = info.target + QStringLiteral(".bak");
    QFile::remove(fresh);
    {
        QFile f(fresh);
        if (!f.open(QIODevice::WriteOnly)) return fail(QStringLiteral("cannot write %1").arg(fresh));
        if (f.write(blob) != blob.size()) {
            f.close();
            QFile::remove(fresh);
            return fail(QStringLiteral("short write to %1 — disk full?").arg(fresh));
        }
        f.close();
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup |
                         QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther);
    }

    // Keep the old one until the new one is in place. On Unix you may rename over a
    // RUNNING binary (the inode survives for us), so this is safe to do live — but
    // if anything below fails the user must still have a working tool.
    QFile::remove(backup);
    if (!QFile::rename(info.target, backup)) {
        QFile::remove(fresh);
        return fail(QStringLiteral("could not move the old binary aside"));
    }
    if (!QFile::rename(fresh, info.target)) {
        QFile::rename(backup, info.target);  // put it back — never leave them with nothing
        QFile::remove(fresh);
        return fail(QStringLiteral("could not put the new binary in place (restored the old one)"));
    }
    QFile::remove(backup);
    return true;
}

}  // namespace odv
