// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Backend.h"

#include <QHash>
#include <QMutex>

#include "CliBackend.h"
#include "OllamaBackend.h"

namespace odv {

namespace {

QMutex g_mutex;
QHash<QString, BackendPtr> g_cache;

}  // namespace

QStringList Backends::all() {
    // Ollama first: it is the only local, free, offline-capable provider and the
    // reason this project exists. The CLIs follow in the order they are listed.
    QStringList ids{QStringLiteral("ollama")};
    ids += CliBackend::ids();
    return ids;
}

BackendPtr Backends::get(const QString& id) {
    // Backends are shared across crew worker threads and are cheap to keep but
    // not free to build (a CliBackend caches its PATH probe, OllamaBackend its
    // /api/show lookups), so they are created once and handed out by pointer.
    QMutexLocker lock(&g_mutex);
    auto it = g_cache.constFind(id);
    if (it != g_cache.constEnd()) return it.value();

    BackendPtr b;
    if (id == QLatin1String("ollama")) {
        b = std::make_shared<OllamaBackend>();
    } else if (CliBackend::ids().contains(id)) {
        b = std::make_shared<CliBackend>(id);
    } else {
        return nullptr;  // unknown id: the caller decides what to do about it
    }

    g_cache.insert(id, b);
    return b;
}

QStringList Backends::availableIds() {
    QStringList out;
    const QStringList ids = all();
    for (const QString& id : ids) {
        // available() is a live probe (HTTP for Ollama, PATH for the CLIs); both
        // cache internally, so this stays cheap enough to call from the UI.
        BackendPtr b = get(id);
        if (b && b->available()) out << id;
    }
    return out;
}

QString Backends::labelFor(const QString& id) {
    if (id == QLatin1String("ollama")) return QStringLiteral("Ollama");
    // Deliberately does NOT go through get(): a label is needed for backends
    // that are not installed (to grey them out in a picker), and constructing
    // one just to ask its name would be backwards.
    if (CliBackend::ids().contains(id)) return CliBackend::labelFor(id);
    return id;
}

}  // namespace odv
