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
#include "PaneHost.h"

namespace odv {

// Ctrl-K fuzzy command palette — a port of app.js CommandPalette (~2761).
// install() puts a Ctrl-K QShortcut on host.window() and builds a lazily-shown
// overlay: a search field over a filtered list of commands (open any pane, center
// the canvas, switch theme). ↑/↓ move, Enter runs, Esc closes.
class CommandPalette {
public:
    static void install(PaneHost& host);
};

}  // namespace odv
