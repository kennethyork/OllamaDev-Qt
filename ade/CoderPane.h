#pragma once
#include "PaneRegistry.h"

namespace odv {

// "👷 Coders" — one live tile per coder in the running crew: what it is doing
// right now, what it has touched, and a box to talk to it. Registered in Panes.cpp.
PaneSpec makeCoderPaneSpec();

}  // namespace odv
