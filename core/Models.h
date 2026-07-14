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
#include <QStringList>
#include <QVector>

namespace odv {

// One catalogued model. `tools` means the model drives NATIVE function calling
// reliably — the only tool path OllamaDev has. A model without it can chat, but
// the agent loop will route tool turns to something that can.
struct Preset {
    QString alias;
    QString tag;
    QString size;  // human-readable download size; empty for cloud (nothing local)
    bool tools = false;
    bool vision = false;
    QString role;
    QString note;
};

// MODELS — a curated catalog plus the fallback/escalation logic around it.
//
// Local models vary wildly in how well they drive tools, and users cannot be
// expected to guess. The catalog is opinionated on purpose; everything else here
// answers two questions: "which installed model should we actually use?" and
// "what do we retry on when this one wasn't good enough?".
//
// Inspects only what is already installed — pulling is a separate, explicit act.
class Models {
public:
    static QVector<Preset> presets();

    // Ollama-hosted models: run on ollama.com, reached through the LOCAL daemon,
    // same API and same `-m <tag>`. Kept out of the local chain so they stay
    // strictly opt-in — a prompt to a cloud model leaves the machine.
    static QVector<Preset> cloudPresets();

    // Cloud tags end in `-cloud` (qwen3-coder:480b-cloud) or `:cloud` (glm-4.6:cloud).
    static bool isCloud(const QString& tag);

    // First installed cloud tag, or empty.
    static QString firstCloud(const QStringList& models);

    // Match a wanted tag against installed ones: exact, then ":latest", then the
    // family prefix. Returns the INSTALLED tag, or empty when nothing matches.
    static QString match(const QString& want, const QStringList& installed);

    // Catalogued native-tool support for a tag's family. -1 = unknown, 0/1 = no/yes.
    static int toolsSupported(const QString& tag);

    // Preferred order for the agentic path, most to least preferred.
    static QStringList defaultChain();

    // Configured chain (model.fallback), else defaultChain().
    static QStringList chain();

    // Parameter count in billions parsed from a tag (qwen2.5-coder:14b -> 14).
    // MoE tags are experts x size (8x7b -> 56), which is what actually has to fit
    // in memory. Returns 0 when the tag carries no size and the family is unknown.
    static double paramSizeB(const QString& tag);

    // Next-bigger INSTALLED model to retry a failed task on, or empty. A configured
    // ladder (models.escalation, small->large) wins; otherwise it is the smallest
    // installed model strictly larger than `current`.
    static QString escalate(const QString& current, const QStringList& installed);

    // First chain entry that is actually installed, or empty.
    static QString bestInstalled(const QStringList& installed);

    // An installed, tool-capable model that is NOT `current`. Used when the active
    // model turns out to have no tool capability at all.
    static QString toolFallback(const QStringList& installed, const QString& current);

    // Resolve a CLI argument (alias or tag) to a concrete tag to pull.
    static QString resolveTag(const QString& nameOrAlias);
};

}  // namespace odv
