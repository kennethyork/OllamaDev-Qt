// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Router.h"

#include <QRegularExpression>

#include "Backend.h"
#include "Config.h"
#include "Models.h"

namespace odv {

namespace {

// Words that mark a genuinely hard software task — the kind worth a strong model.
const QStringList& hardWords() {
    static const QStringList w{
        "design",     "architect",   "architecture", "refactor",  "implement",
        "algorithm",  "optimize",    "optimise",     "debug",      "diagnose",
        "prove",      "migrate",     "concurrency",  "thread",     "race condition",
        "security",   "vulnerab",    "port ",        "rewrite",    "profile",
        "benchmark",  "distributed", "recursive",    "parser",     "compiler",
        "state machine"};
    return w;
}

// Openers that mark a trivial ask — a small model is plenty.
const QStringList& simpleWords() {
    static const QStringList w{"what is",    "what's",   "who is",   "when ",    "where ",
                               "define",     "rename",   "list ",    "spell",    "translate",
                               "summarize",  "summarise", "tldr",    "one line", "briefly",
                               "hello",      "hi ",      "thanks",   "capital of"};
    return w;
}

QString cfg(const QString& key, const QString& fallback) {
    const QString v = Config::str(key);
    return v.isEmpty() ? fallback : v;
}

QStringList installed() {
    auto b = Backends::get(QStringLiteral("ollama"));
    return b ? b->models() : QStringList{};
}

// The smallest installed LOCAL model by parameter count — the cheap tier's default.
QString smallestLocal(const QStringList& models) {
    QString best;
    double bestSize = 1e9;
    for (const QString& m : models) {
        if (Models::isCloud(m)) continue;
        const double s = Models::paramSizeB(m);
        if (s > 0 && s < bestSize) {
            bestSize = s;
            best = m;
        }
    }
    return best;
}

}  // namespace

QString Router::modelForTier(const QString& tier) {
    const QStringList models = installed();
    const QString sessionDefault = Config::str(QStringLiteral("ollama.defaultModel"));

    if (tier == QLatin1String("simple")) {
        // A configured pick wins; else the smallest local model; else the session default.
        const QString def = smallestLocal(models);
        return cfg(QStringLiteral("router.simple"), def.isEmpty() ? sessionDefault : def);
    }
    if (tier == QLatin1String("hard")) {
        // Prefer a cloud model (offloads the heavy turn to Ollama's servers), else
        // the biggest thing installed locally.
        QString def = Models::firstCloud(models);
        if (def.isEmpty()) def = Models::bestInstalled(models);
        return cfg(QStringLiteral("router.hard"), def.isEmpty() ? sessionDefault : def);
    }
    // moderate
    QString def = sessionDefault.isEmpty() ? Models::bestInstalled(models) : sessionDefault;
    return cfg(QStringLiteral("router.moderate"), def);
}

QString Router::classify(const QString& prompt, QString* reason) {
    const QString p = prompt.trimmed().toLower();
    auto say = [&](const QString& r) {
        if (reason) *reason = r;
    };

    // A fenced code block, or a long multi-part ask, is hard regardless of words:
    // the model has to reason over real code, not answer a one-liner.
    if (p.contains(QStringLiteral("```")) || p.contains(QStringLiteral("    "))) {
        say(QStringLiteral("contains code"));
        return QStringLiteral("hard");
    }
    for (const QString& w : hardWords()) {
        if (p.contains(w)) {
            say(QStringLiteral("mentions \"%1\"").arg(w.trimmed()));
            return QStringLiteral("hard");
        }
    }
    if (p.length() > 400) {
        say(QStringLiteral("long / detailed request"));
        return QStringLiteral("hard");
    }

    // Short and opens like a lookup → simple. Length gate first so "design a…"
    // never slips through on brevity.
    if (p.length() <= 80) {
        for (const QString& w : simpleWords()) {
            if (p.startsWith(w) || p.contains(w)) {
                say(QStringLiteral("short lookup-style question"));
                return QStringLiteral("simple");
            }
        }
        // Pure arithmetic / very short.
        if (p.length() <= 24) {
            say(QStringLiteral("very short"));
            return QStringLiteral("simple");
        }
    }

    say(QStringLiteral("general request"));
    return QStringLiteral("moderate");
}

RouteDecision Router::pick(const QString& prompt) {
    RouteDecision d;
    d.backend = QStringLiteral("ollama");
    d.tier = classify(prompt, &d.reason);
    d.model = modelForTier(d.tier);
    return d;
}

}  // namespace odv
