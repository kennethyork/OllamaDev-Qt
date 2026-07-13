#include "PaneRegistry.h"

// Each extra pane exposes a `PaneSpec makeXxxPaneSpec()` in its own header; this
// file is the ONE place that lists them. A new pane = a new file + one include +
// one add() line here. Keep it that way — it is what keeps panes from having to
// edit MainWindow, and the single shared edit point so parallel pane work does
// not collide.

#include "AgentTeamPane.h"
#include "BrowserPane.h"
#include "CodeSearchPane.h"
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
    r.add(makeAgentTeamPaneSpec());
}

}  // namespace odv
