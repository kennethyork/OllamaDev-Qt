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

// Each extra pane exposes a `PaneSpec makeXxxPaneSpec()` in its own header; this
// file is the ONE place that lists them. A new pane = a new file + one include +
// one add() line here. Keep it that way — it is what keeps panes from having to
// edit MainWindow, and the single shared edit point so parallel pane work does
// not collide.

#include "AgentTeamPane.h"
#include "BrainPane.h"
#include "BrowserPane.h"
#include "ChatPane.h"
#include "CodeSearchPane.h"
#include "CoderPane.h"
#include "GitPane.h"
#include "GraphPane.h"
#include "StartPane.h"
#include "TasksPane.h"
#include "TopologyPane.h"
#include "VoicePane.h"

namespace odv {

void registerExtraPanes() {
    auto& r = PaneRegistry::instance();
    r.add(makeStartPaneSpec());
    r.add(makeTasksPaneSpec());
    r.add(makeGraphPaneSpec());
    r.add(makeBrowserPaneSpec());
    r.add(makeCodeSearchPaneSpec());
    r.add(makeVoicePaneSpec());
    r.add(makeTopologyPaneSpec());
    r.add(makeCoderPaneSpec());
    r.add(makeChatPaneSpec());
    r.add(makeGitPaneSpec());
    r.add(makeAgentTeamPaneSpec());
    r.add(makeBrainPaneSpec());
}

}  // namespace odv
