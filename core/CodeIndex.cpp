// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "CodeIndex.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSet>
#include <QScopedPointer>
#include <QUrl>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

#include "Backend.h"
#include "Config.h"
#include "Json.h"
#include "Models.h"
#include "Parallel.h"

namespace odv {
namespace {

constexpr int kChunkLines = 60;
constexpr int kOverlap = 12;
constexpr qint64 kMaxFileBytes = 200000;
constexpr int kMaxEmbedChars = 8000;   // the embedding model's window, not ours
constexpr int kSnippetChars = 400;

const QStringList& ignoreDirs() {
    static const QStringList d{
        QStringLiteral(".git"),    QStringLiteral(".build"),      QStringLiteral("node_modules"),
        QStringLiteral("vendor"),  QStringLiteral(".ollamadev"),  QStringLiteral("dist"),
        QStringLiteral("build"),   QStringLiteral("out"),         QStringLiteral(".venv"),
        QStringLiteral("venv"),    QStringLiteral("__pycache__"), QStringLiteral(".next"),
        QStringLiteral("target"),  QStringLiteral("coverage"),    QStringLiteral(".cache"),
        QStringLiteral(".idea"),   QStringLiteral(".vscode")};
    return d;
}

const QStringList& textExt() {
    static const QStringList e{
        QStringLiteral("php"),  QStringLiteral("js"),    QStringLiteral("mjs"),  QStringLiteral("cjs"),
        QStringLiteral("ts"),   QStringLiteral("tsx"),   QStringLiteral("jsx"),  QStringLiteral("py"),
        QStringLiteral("go"),   QStringLiteral("rs"),    QStringLiteral("rb"),   QStringLiteral("java"),
        QStringLiteral("c"),    QStringLiteral("h"),     QStringLiteral("cpp"),  QStringLiteral("cc"),
        QStringLiteral("hpp"),  QStringLiteral("cs"),    QStringLiteral("swift"),QStringLiteral("kt"),
        QStringLiteral("kts"),  QStringLiteral("scala"), QStringLiteral("css"),  QStringLiteral("scss"),
        QStringLiteral("less"), QStringLiteral("html"),  QStringLiteral("htm"),  QStringLiteral("vue"),
        QStringLiteral("svelte"), QStringLiteral("json"),QStringLiteral("jsonc"),QStringLiteral("md"),
        QStringLiteral("mdx"),  QStringLiteral("yml"),   QStringLiteral("yaml"), QStringLiteral("toml"),
        QStringLiteral("ini"),  QStringLiteral("sh"),    QStringLiteral("bash"), QStringLiteral("zsh"),
        QStringLiteral("sql"),  QStringLiteral("graphql"), QStringLiteral("proto"), QStringLiteral("lua"),
        QStringLiteral("pl"),   QStringLiteral("pm"),    QStringLiteral("r"),    QStringLiteral("dart"),
        QStringLiteral("ex"),   QStringLiteral("exs"),   QStringLiteral("cmake"), QStringLiteral("txt")};
    return e;
}

QString indexFile() { return CodeIndex::dir() + QStringLiteral("/code.json"); }

QString ollamaHost() {
    QString h = Config::str(QStringLiteral("ollama.host"), QStringLiteral("http://localhost:11434"));
    while (h.endsWith(QLatin1Char('/'))) h.chop(1);
    return h;
}

// A chunk before it has been embedded.
struct RawChunk {
    QString file;
    int start = 0;
    int end = 0;
    QString text;
};

double cosine(const QVector<float>& a, const QVector<float>& b) {
    const int n = std::min(a.size(), b.size());
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (int i = 0; i < n; ++i) {
        dot += double(a[i]) * double(b[i]);
        na += double(a[i]) * double(a[i]);
        nb += double(b[i]) * double(b[i]);
    }
    const double d = std::sqrt(na) * std::sqrt(nb);
    return d > 0.0 ? dot / d : 0.0;
}

// Repo-relative paths worth indexing.
QStringList walk(const QString& root) {
    QStringList out;
    QDir base(root);
    QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString rel = base.relativeFilePath(path);

        bool skip = false;
        const QStringList segs = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (int i = 0; i + 1 < segs.size(); ++i) {  // directory components only
            if (ignoreDirs().contains(segs.at(i))) {
                skip = true;
                break;
            }
        }
        if (skip) continue;

        const QFileInfo fi(path);
        if (!textExt().contains(fi.suffix().toLower())) continue;
        if (fi.size() > kMaxFileBytes) continue;
        out << rel;
    }
    out.sort();
    return out;
}

// The admission key the crew uses for the same server. A local embedding model
// competes for the very same OLLAMA_NUM_PARALLEL slots as a local coder, so they
// must share one semaphore; a :cloud model does not touch our GPU at all.
QString limiterKey(const QString& m) {
    return Models::isCloud(m) ? QStringLiteral("ollama:cloud") : QStringLiteral("ollama:local");
}

// How wide we may fan out. Only ever RAISES the shared ceiling: lowering it would
// block inside setLimit() until a running crew handed its permits back.
int admitWidth(const QString& m) {
    auto ollama = Backends::get(QStringLiteral("ollama"));
    const int want = ollama ? std::max(1, ollama->concurrencyLimit(m)) : 1;
    const QString key = limiterKey(m);
    if (Limiter::instance().limit(key) < want) Limiter::instance().setLimit(key, want);
    return want;
}

}  // namespace

QString CodeIndex::model() {
    const QString m = Config::str(QStringLiteral("embed.model")).trimmed();
    return m.isEmpty() ? QStringLiteral("nomic-embed-text") : m;
}

QString CodeIndex::dir() {
    const QString d = Config::dataDir() + QStringLiteral("/index");
    QDir().mkpath(d);
    return d;
}

QVector<float> CodeIndex::embed(const QString& text) {
    QJsonObject req;
    req.insert(QStringLiteral("model"), model());
    req.insert(QStringLiteral("prompt"), text.left(kMaxEmbedChars));

    // Manager + loop are frame-local, which is what makes this callable from the
    // build's worker threads (QNetworkAccessManager is thread-affine).
    QNetworkAccessManager nam;
    QNetworkRequest r{QUrl(ollamaHost() + QStringLiteral("/api/embeddings"))};
    r.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    r.setTransferTimeout(60000);

    QEventLoop loop;
    QScopedPointer<QNetworkReply> reply(nam.post(r, json::encode(req)));
    QObject::connect(reply.data(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) return {};
    const QJsonArray arr = QJsonDocument::fromJson(reply->readAll())
                               .object()
                               .value(QStringLiteral("embedding"))
                               .toArray();
    QVector<float> vec;
    vec.reserve(arr.size());
    for (const QJsonValue& v : arr) vec.append(static_cast<float>(v.toDouble()));
    return vec;
}

BuildReport CodeIndex::build(const Progress& progress) {
    BuildReport rep;
    const QString root = QDir::currentPath();
    const QString m = model();

    // Prove the model answers ONCE, up front. Otherwise a missing model produces
    // one failed request per chunk and a "0 chunks" report that says nothing.
    if (embed(QStringLiteral("ping")).isEmpty()) {
        rep.error = QStringLiteral("embed_failed");
        return rep;
    }

    const QStringList files = walk(root);
    rep.files = files.size();

    QVector<RawChunk> raw;
    for (const QString& rel : files) {
        QFile f(root + QLatin1Char('/') + rel);
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QString src = QString::fromUtf8(f.readAll());
        f.close();
        if (src.trimmed().isEmpty()) continue;

        const QStringList lines = src.split(QLatin1Char('\n'));
        const int total = lines.size();
        const int step = std::max(1, kChunkLines - kOverlap);
        for (int i = 0; i < total; i += step) {
            const int take = std::min(kChunkLines, total - i);
            const QString text = lines.mid(i, take).join(QLatin1Char('\n')).trimmed();
            if (!text.isEmpty()) {
                RawChunk c;
                c.file = rel;
                c.start = i + 1;
                c.end = std::min(i + kChunkLines, total);
                c.text = text;
                raw.append(c);
            }
            if (i + kChunkLines >= total) break;
        }
    }
    if (raw.isEmpty()) {
        rep.ok = true;  // an empty repo is not a failure
        return rep;
    }

    // ---- concurrent embedding -------------------------------------------------
    const QString key = limiterKey(m);
    const int count = static_cast<int>(raw.size());
    const int width = std::min(admitWidth(m), count);

    QVector<QVector<float>> vecs(count);
    std::atomic_int next{0};
    std::atomic_int done{0};
    QMutex progressMutex;

    auto worker = [&]() {
        for (;;) {
            const int i = next.fetch_add(1);
            if (i >= count) return;

            const QString payload = raw[i].file + QLatin1Char('\n') + raw[i].text;
            {
                // The permit is held for ONE request, not for the whole worker, so a
                // crew coder queued behind us gets a slot as soon as this call lands
                // instead of waiting for the entire index to finish.
                auto permit = Limiter::instance().acquire(key);
                vecs[i] = embed(payload);
                if (vecs[i].isEmpty()) vecs[i] = embed(payload);  // one retry: the model is live
            }
            const int n = ++done;
            if (progress) {
                QMutexLocker lock(&progressMutex);
                progress(raw[i].file, n, count);
            }
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(width));
    for (int t = 0; t < width; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();

    // ---- persist --------------------------------------------------------------
    QJsonArray chunks;
    int dim = 0;
    for (int i = 0; i < count; ++i) {
        if (vecs[i].isEmpty()) {
            ++rep.skipped;
            continue;
        }
        if (dim == 0) dim = static_cast<int>(vecs[i].size());
        QJsonArray vec;
        for (float f : vecs[i]) vec.append(double(f));
        chunks.append(QJsonObject{{"file", raw[i].file},
                                  {"start", raw[i].start},
                                  {"end", raw[i].end},
                                  {"text", raw[i].text.left(kSnippetChars)},
                                  {"vec", vec}});
    }
    if (chunks.isEmpty()) {
        rep.error = QStringLiteral("embed_failed");
        return rep;
    }

    const QJsonObject data{
        {"model", m},
        {"root", root},
        {"built", QDateTime::currentDateTime().toString(Qt::ISODate)},
        {"dim", dim},
        {"chunks", chunks}};

    // Temp file + rename: a crash mid-write must not leave a half-written index
    // that the next `code-search` chokes on.
    QSaveFile out(indexFile());
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        rep.error = QStringLiteral("cannot write %1").arg(indexFile());
        return rep;
    }
    out.write(QJsonDocument(data).toJson(QJsonDocument::Compact));
    if (!out.commit()) {
        rep.error = QStringLiteral("cannot write %1").arg(indexFile());
        return rep;
    }

    rep.ok = true;
    rep.chunks = static_cast<int>(chunks.size());
    return rep;
}

SearchReport CodeIndex::search(const QString& query, int limit) {
    SearchReport rep;

    QFile f(indexFile());
    if (!f.open(QIODevice::ReadOnly)) {
        rep.error = QStringLiteral("no_index");
        return rep;
    }
    const QJsonObject data = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    const QJsonArray chunks = data.value(QStringLiteral("chunks")).toArray();
    if (chunks.isEmpty()) {
        rep.error = QStringLiteral("no_index");
        return rep;
    }

    const QVector<float> qv = embed(query);
    if (qv.isEmpty()) {
        rep.error = QStringLiteral("embed_failed");
        return rep;
    }

    QVector<IndexHit> scored;
    scored.reserve(chunks.size());
    for (const QJsonValue& cv : chunks) {
        const QJsonObject c = cv.toObject();
        const QJsonArray va = c.value(QStringLiteral("vec")).toArray();
        QVector<float> vec;
        vec.reserve(va.size());
        for (const QJsonValue& v : va) vec.append(static_cast<float>(v.toDouble()));

        IndexHit h;
        h.file = c.value(QStringLiteral("file")).toString();
        h.start = c.value(QStringLiteral("start")).toInt();
        h.end = c.value(QStringLiteral("end")).toInt();
        h.snippet = c.value(QStringLiteral("text")).toString();
        h.score = cosine(qv, vec);
        scored.append(h);
    }

    std::sort(scored.begin(), scored.end(),
              [](const IndexHit& a, const IndexHit& b) { return a.score > b.score; });
    rep.hits = scored.mid(0, std::max(1, limit));
    rep.ok = true;
    return rep;
}

IndexStatus CodeIndex::status() {
    IndexStatus s;
    QFile f(indexFile());
    if (!f.open(QIODevice::ReadOnly)) return s;
    const QJsonObject data = QJsonDocument::fromJson(f.readAll()).object();
    f.close();

    const QJsonArray chunks = data.value(QStringLiteral("chunks")).toArray();
    if (chunks.isEmpty()) return s;

    QSet<QString> files;
    for (const QJsonValue& c : chunks) files.insert(c.toObject().value(QStringLiteral("file")).toString());

    s.exists = true;
    s.model = data.value(QStringLiteral("model")).toString();
    s.built = data.value(QStringLiteral("built")).toString();
    s.root = data.value(QStringLiteral("root")).toString();
    s.dim = data.value(QStringLiteral("dim")).toInt();
    s.files = static_cast<int>(files.size());
    s.chunks = static_cast<int>(chunks.size());
    return s;
}

bool CodeIndex::clear() {
    const QString f = indexFile();
    if (!QFileInfo::exists(f)) return true;
    return QFile::remove(f);
}

}  // namespace odv
