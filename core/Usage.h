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
#include <QMap>
#include <QString>
namespace odv {
// Token/usage meter from /api/chat prompt_eval_count / eval_count.
class Usage {
public:
    struct Tally {
        qint64 prompt = 0;
        qint64 eval = 0;
        qint64 total() const { return prompt + eval; }
    };

    static void record(const QString& model, int promptTokens, int evalTokens);
    static QString report();  // human-readable cumulative

    // Per-model totals right now, for computing a delta across a crew run.
    static QMap<QString, Tally> snapshot();
};
}  // namespace odv
