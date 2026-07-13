#include "AgentTeamPane.h"

#include <QLabel>

namespace odv {

PaneSpec makeAgentTeamPaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("agentteam");
    s.title = QStringLiteral("Agent team");
    s.group = QStringLiteral("Crew");
    s.singleton = true;
    s.factory = [](PaneHost&) -> QWidget* {
        // STUB placeholder — the real pane replaces this.
        return new QLabel(QStringLiteral("Agent team — not yet ported"));
    };
    return s;
}

}  // namespace odv
