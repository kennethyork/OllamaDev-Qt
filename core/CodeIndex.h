// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <QString>
#include <QVector>
#include <functional>

namespace odv {

struct IndexHit {
    QString file;
    int start = 0;  // 1-based first line of the chunk
    int end = 0;
    double score = 0.0;  // cosine similarity, -1..1
    QString snippet;
};

struct BuildReport {
    bool ok = false;
    int files = 0;
    int chunks = 0;
    int skipped = 0;  // chunks whose embedding failed twice
    QString error;    // "embed_failed" when the model never answered
};

struct SearchReport {
    bool ok = false;
    QVector<IndexHit> hits;
    QString error;  // "no_index" | "embed_failed"
};

struct IndexStatus {
    bool exists = false;
    QString model;
    QString built;
    QString root;
    int dim = 0;
    int files = 0;
    int chunks = 0;
};

// Local semantic code search. Chunks the repo, embeds each chunk with a local
// Ollama embedding model (default nomic-embed-text) and ranks chunks against a
// query by cosine similarity. 100% local: embeddings run on the user's own
// Ollama, nothing leaves the machine.
//
// Unlike the PHP original, which embedded chunks one blocking request at a time,
// build() fans the requests out across threads. The fan-out is admitted by the
// SHARED Limiter under the same "ollama:local"/"ollama:cloud" key the crew uses,
// so indexing a repo while a crew is running cannot double-book the GPU's
// OLLAMA_NUM_PARALLEL slots — the two simply take turns.
class CodeIndex {
public:
    static QString model();  // embed.model, default nomic-embed-text
    static QString dir();    // Config::dataDir()/index

    // One text → its embedding. Empty on failure (model not installed, Ollama down).
    static QVector<float> embed(const QString& text);

    // progress(file, done, total) is called from worker threads — it must be
    // thread-safe or cheap to serialise; CodeIndex holds a lock while calling it.
    using Progress = std::function<void(const QString& file, int done, int total)>;
    static BuildReport build(const Progress& progress = {});

    static SearchReport search(const QString& query, int limit = 8);
    static IndexStatus status();
    static bool clear();
};

}  // namespace odv
