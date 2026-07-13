#pragma once
#include <QWidget>
#include "PaneHost.h"

namespace odv {

// Ctrl-K fuzzy command palette. STUB — replace with the real overlay.
// Installed by MainWindow via CommandPalette::install(host).
class CommandPalette {
public:
    static void install(PaneHost& host);
};

}  // namespace odv
