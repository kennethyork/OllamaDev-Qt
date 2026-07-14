#pragma once
#include "PaneRegistry.h"

namespace odv {

// "Git" — a real git client on the canvas: branches, staged/unstaged changes,
// an inline diff, AI-written commit messages, and history. Registered in Panes.cpp.
PaneSpec makeGitPaneSpec();

}  // namespace odv
