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
