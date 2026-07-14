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
#include <QJsonObject>
#include <QString>
#include <QVector>

#include "Tools.h"  // ToolDef

namespace odv {
// Nested agent (the `task` tool): runs a focused sub-task in a fresh Agent with
// its own short message list, READ-ONLY by default, and a hard recursion cap so a
// delegating model cannot spawn agents without bound.
class SubAgent {
public:
    // Run a delegated task from structured arguments (prompt/task, context,
    // agent_type, permission, max_iterations). Returns a concise result string.
    static QString run(const QJsonObject& args);

    // Convenience overload: a bare task string.
    static QString run(const QString& task, int depth = 0);

    // The `task`/`subagent`/`delegate` tool definitions. Tools::registerAll() adds
    // these to the registry (the registry's add() is file-local to Tools.cpp, so
    // registration is centralised there exactly as it is for MCP tools).
    static QVector<ToolDef> tools();

    static constexpr int kMaxDepth = 2;  // hard recursion cap
};
}  // namespace odv
