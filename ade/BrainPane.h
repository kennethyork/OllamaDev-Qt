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
#include "PaneRegistry.h"

namespace odv {

// The routing "brain", visualised: the tier→model map, a live classifier you can
// type into, the current crew's routed model plan, and the free-vs-paid token
// split. Registered in Panes.cpp.
PaneSpec makeBrainPaneSpec();

}  // namespace odv
