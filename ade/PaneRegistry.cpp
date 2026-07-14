// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PaneRegistry.h"

namespace odv {

PaneRegistry& PaneRegistry::instance() {
    static PaneRegistry r;
    return r;
}

void PaneRegistry::add(const PaneSpec& spec) {
    // Last registration of a kind wins, so a pane can be overridden in tests.
    for (auto& s : specs_) {
        if (s.kind == spec.kind) {
            s = spec;
            return;
        }
    }
    specs_.append(spec);
}

const PaneSpec* PaneRegistry::find(const QString& kind) const {
    for (const auto& s : specs_)
        if (s.kind == kind) return &s;
    return nullptr;
}

}  // namespace odv
