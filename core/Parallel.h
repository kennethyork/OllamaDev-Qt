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
#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QSemaphore>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>

namespace odv {

// Per-backend admission control.
//
// The whole point of the C++ rewrite's crew is that N coders run at once. But
// "at once" means different things per provider:
//
//   local Ollama, one GPU  — the server has OLLAMA_NUM_PARALLEL slots per loaded
//                            model, and each slot costs a full num_ctx KV cache.
//                            Exceeding it does not go faster; requests just queue
//                            inside Ollama while we hold RAM hostage. So we cap
//                            at the server's real slot count.
//   Ollama cloud (:cloud)  — inference happens on Ollama's servers. No local GPU
//                            contention, so we can fan out much wider.
//   claude/codex/gemini/…  — separate processes hitting separate remote APIs.
//                            Bounded by rate limits, not by our hardware.
//
// Limiter hands out permits keyed by backend id so a crew mixing a local coder
// with three cloud coders throttles each one correctly and independently.
class Limiter {
public:
    static Limiter& instance();

    // Blocks until a slot for `key` is free. Releasing is RAII via Permit.
    class Permit {
    public:
        Permit(Limiter* owner, QString key) : owner_(owner), key_(std::move(key)) {}
        ~Permit();
        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;
        Permit(Permit&& o) noexcept : owner_(o.owner_), key_(std::move(o.key_)) { o.owner_ = nullptr; }

    private:
        Limiter* owner_;
        QString key_;
    };

    // Declare (or lower) the concurrency ceiling for a key. Idempotent.
    void setLimit(const QString& key, int limit);
    int limit(const QString& key);

    [[nodiscard]] Permit acquire(const QString& key);
    void release(const QString& key);

private:
    QMutex mutex_;
    QHash<QString, std::shared_ptr<QSemaphore>> sems_;
    QHash<QString, int> limits_;
};

// Run `fn` over every item concurrently, respecting each item's limiter key.
// Returns results in input order. Exceptions in `fn` become default-constructed
// results — the caller checks the result's own ok/error field.
//
// Unlike a wave pool (chunk + join), this is a true work pool: a fast coder
// starts the next subtask while a slow one is still thinking.
template <typename In, typename Out>
QVector<Out> parallelMap(const QVector<In>& items,
                         const std::function<QString(const In&)>& keyOf,
                         const std::function<Out(const In&, int index)>& fn);

// Non-template entry point used by Crew (keeps the template out of the ABI).
QVector<QJsonObject> parallelRun(
    int count,
    const std::function<QString(int)>& keyOf,
    const std::function<QJsonObject(int)>& fn);

}  // namespace odv
