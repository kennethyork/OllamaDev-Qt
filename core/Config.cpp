// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Config.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>

#include "Json.h"

namespace odv {

QJsonObject Config::merged_;
bool Config::loaded_ = false;

namespace {

// The whole config is read by every worker thread (backends, crew coders) and
// written by setPref from the UI thread. One lock over the cache is enough —
// reads are cheap and the file layers are only touched on the first load.
// Recursive because every accessor calls load() first, which takes it too.
QRecursiveMutex& lock() {
    static QRecursiveMutex m;
    return m;
}

QString envStr(const char* name) {
    return QString::fromLocal8Bit(qgetenv(name)).trimmed();
}

QString homePath() {
    const QString h = QDir::homePath();
    return h.isEmpty() ? QStringLiteral("/tmp") : h;
}

QString prefsFile() {
    return homePath() + QStringLiteral("/.ollamadev/ade-prefs.json");
}

QJsonObject readJsonObject(const QString& path) {
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return {};
    const QByteArray raw = f.readAll();
    f.close();
    // Hand-edited config files pick up stray fences/prose exactly like model
    // output does, so parse them the forgiving way rather than losing the file.
    return json::objectFrom(QString::fromUtf8(raw));
}

QJsonObject defaults() {
    QJsonObject ollama{
        {"host", QStringLiteral("http://localhost:11434")},
        {"hosts", QJsonArray{}},
        {"defaultModel", QStringLiteral("qwen3.5:9b")},
        {"contextWindow", 16384},
        {"maxContextWindow", 32768},
        {"autoContext", true},
        {"temperature", 0.3},
        {"stream", true},
        // NO `lowResource` default here, deliberately.
        //
        // It used to be a hardcoded `true`: light mode — small KV cache, model
        // unloaded when idle, coders serialised. Safe, and completely wrong for
        // anyone with a real GPU: a 24GB RTX 3090 got capped at an 8192-token
        // context exactly as hard as a laptop with no GPU at all, even when the
        // user's own contextWindow asked for 16384 and the model could do 262144.
        //
        // Leaving the key ABSENT is what lets OllamaBackend fall back to
        // ContextTuner::lowResourceMachine(), which measures the hardware. Put a
        // default back here and you silently kill that, because Config::boolean()
        // finds the default and never reaches the fallback.
        {"keepAlive", QStringLiteral("60s")},
    };

    QJsonObject crew{
        {"maxCoders", 4},
        {"coderIterations", 10},
        {"land", QStringLiteral("auto")},
        {"research", true},
        {"audit", true},
    };

    QJsonObject agents{
        {"coder", QJsonObject{{"temperature", 0.7}, {"maxTokens", 4096}}},
        {"maxIterations", 12},
        {"maxToolOutput", 12000},
        {"subagentPermission", QStringLiteral("readonly")},
    };

    QJsonObject cliEntry;  // one shape per coding CLI: binary + optional model pin
    auto cli = [&](const char* command) {
        return QJsonObject{{"command", QString::fromLatin1(command)}, {"defaultModel", QString()}};
    };
    cliEntry.insert("codex", cli("codex"));
    cliEntry.insert("claude", cli("claude"));
    cliEntry.insert("opencode", cli("opencode"));
    cliEntry.insert("qwen", cli("qwen"));

    return QJsonObject{
        {"model", QJsonObject{{"backend", QStringLiteral("ollama")}}},
        {"ollama", ollama},
        {"cli", cliEntry},
        {"agents", agents},
        {"crew", crew},
        {"session", QJsonObject{{"autoResume", true}}},
        {"memory", QJsonObject{{"autoRemember", true}}},
        {"data", QJsonObject{{"directory", QStringLiteral(".ollamadev")}}},
    };
}

// Documented env vars are overrides: they must beat anything on disk, otherwise
// `OLLAMA_HOST=... ollamadev` would silently talk to the configured host.
QJsonObject envOverlay() {
    QJsonObject flat;

    const QString host = envStr("OLLAMA_HOST");
    if (!host.isEmpty()) flat.insert("ollama.host", host);

    const QString apiKey = envStr("OLLAMA_API_KEY");
    if (!apiKey.isEmpty()) flat.insert("ollama.authToken", apiKey);

    const QString model = envStr("OLLAMA_MODEL");
    if (!model.isEmpty()) flat.insert("ollama.defaultModel", model);

    QString backend = envStr("OLLAMADEV_BACKEND");
    if (backend.isEmpty()) backend = envStr("AI_BACKEND");
    if (!backend.isEmpty()) flat.insert("model.backend", backend);

    const QString numCtx = envStr("OLLAMA_NUM_CTX");
    bool ok = false;
    const int ctx = numCtx.toInt(&ok);
    if (ok && ctx > 0) flat.insert("ollama.contextWindow", ctx);

    const QString maxCtx = envStr("OLLAMA_MAX_NUM_CTX");
    ok = false;
    const int maxCtxV = maxCtx.toInt(&ok);
    if (ok && maxCtxV > 0) flat.insert("ollama.maxContextWindow", maxCtxV);

    // OLLAMADEV_POWER is the no-config-file way to pin resource mode for a shell.
    const QString power = envStr("OLLAMADEV_POWER").toLower();
    if (power == QLatin1String("full") || power == QLatin1String("heavy") ||
        power == QLatin1String("high") || power == QLatin1String("off")) {
        flat.insert("ollama.lowResource", false);
    } else if (power == QLatin1String("light") || power == QLatin1String("low") ||
               power == QLatin1String("on")) {
        flat.insert("ollama.lowResource", true);
    }

    return json::expandDotted(flat);
}

}  // namespace

void Config::load() {
    QMutexLocker guard(&lock());
    if (loaded_) return;

    // First existing file wins, same order as the PHP original so a machine that
    // still runs both sees one configuration.
    QJsonObject fileCfg;
    const QStringList paths{
        homePath() + QStringLiteral("/.ollamadev/config.json"),
        homePath() + QStringLiteral("/.config/ollamadev/config.json"),
        QDir::current().filePath(QStringLiteral(".ollamadev.json")),
    };
    for (const QString& p : paths) {
        fileCfg = readJsonObject(p);
        if (!fileCfg.isEmpty()) break;
    }

    // ade-prefs.json holds flat dotted keys ("web.enabled": false) written by the
    // desktop. It overlays config.json so a GUI toggle takes effect everywhere
    // without the GUI ever rewriting the user's (MCP-only) config.json.
    const QJsonObject prefs = json::expandDotted(readJsonObject(prefsFile()));

    merged_ = json::mergeDeep(defaults(), fileCfg);
    merged_ = json::mergeDeep(merged_, prefs);
    merged_ = json::mergeDeep(merged_, envOverlay());
    loaded_ = true;
}

QJsonValue Config::get(const QString& dottedKey, const QJsonValue& fallback) {
    load();
    QMutexLocker guard(&lock());
    return json::at(merged_, dottedKey, fallback);
}

QString Config::str(const QString& dottedKey, const QString& fallback) {
    const QJsonValue v = get(dottedKey);
    if (v.isString()) return v.toString();
    if (v.isDouble()) return QString::number(v.toDouble());
    if (v.isBool()) return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return fallback;
}

int Config::integer(const QString& dottedKey, int fallback) {
    const QJsonValue v = get(dottedKey);
    if (v.isDouble()) return v.toInt(fallback);
    // Values that came from ade-prefs.json or an env var arrive as strings.
    if (v.isString()) {
        bool ok = false;
        const int n = v.toString().trimmed().toInt(&ok);
        if (ok) return n;
    }
    if (v.isBool()) return v.toBool() ? 1 : 0;
    return fallback;
}

double Config::number(const QString& dottedKey, double fallback) {
    const QJsonValue v = get(dottedKey);
    if (v.isDouble()) return v.toDouble(fallback);
    if (v.isString()) {
        bool ok = false;
        const double n = v.toString().trimmed().toDouble(&ok);
        if (ok) return n;
    }
    return fallback;
}

bool Config::boolean(const QString& dottedKey, bool fallback) {
    const QJsonValue v = get(dottedKey);
    if (v.isBool()) return v.toBool();
    if (v.isDouble()) return v.toDouble() != 0.0;
    if (v.isString()) {
        const QString s = v.toString().trimmed().toLower();
        if (s == QLatin1String("true") || s == QLatin1String("1") || s == QLatin1String("yes") ||
            s == QLatin1String("on")) {
            return true;
        }
        if (s == QLatin1String("false") || s == QLatin1String("0") || s == QLatin1String("no") ||
            s == QLatin1String("off")) {
            return false;
        }
    }
    return fallback;
}

void Config::setPref(const QString& dottedKey, const QJsonValue& value) {
    if (dottedKey.isEmpty()) return;
    load();
    QMutexLocker guard(&lock());

    const QString file = prefsFile();
    QDir().mkpath(QFileInfo(file).absolutePath());

    // Read-modify-write: another interface (CLI, desktop, web) may have written
    // a different key since we loaded, and clobbering it would silently undo a
    // setting the user changed elsewhere.
    QJsonObject flat = readJsonObject(file);
    flat.insert(dottedKey, value);

    // Temp file + rename, so a crash mid-write can never leave a half-written
    // prefs file that the next start would fail to parse.
    QSaveFile out(file);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(QJsonDocument(flat).toJson(QJsonDocument::Indented));
        out.commit();
    }

    QJsonObject one;
    one.insert(dottedKey, value);
    merged_ = json::mergeDeep(merged_, json::expandDotted(one));
}

QString Config::homeDir() {
    return homePath() + QStringLiteral("/.ollamadev");
}

QString Config::dataDir() {
    const QString dir = str(QStringLiteral("data.directory"), QStringLiteral(".ollamadev"));
    if (dir.startsWith(u'/')) return dir;
    return QDir::current().filePath(dir);
}

QString Config::crewDir() {
    return homeDir() + QStringLiteral("/crew");
}

QString Config::boardDir() {
    return homeDir() + QStringLiteral("/board");
}

QString Config::terminalsDir() {
    return homeDir() + QStringLiteral("/terminals");
}

}  // namespace odv
