#pragma once
#include "PaneRegistry.h"

namespace odv {

// "Chat" — a plain conversation with a model, on the canvas. NOT a singleton:
// every ＋ Chat is its own pane, its own session, its own model. Registered in
// Panes.cpp.
PaneSpec makeChatPaneSpec();

}  // namespace odv
