#pragma once
#include "PaneRegistry.h"

namespace odv {

// The routing "brain", visualised: the tier→model map, a live classifier you can
// type into, the current crew's routed model plan, and the free-vs-paid token
// split. Registered in Panes.cpp.
PaneSpec makeBrainPaneSpec();

}  // namespace odv
