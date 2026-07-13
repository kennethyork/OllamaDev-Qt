#include "TopologyPane.h"

#include <QLabel>

namespace odv {

PaneSpec makeTopologyPaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("topology");
    s.title = QStringLiteral("🛰 Crew topology");
    s.group = QStringLiteral("Crew");
    s.singleton = true;
    s.factory = [](PaneHost&) -> QWidget* {
        // STUB placeholder — the real pane replaces this.
        return new QLabel(QStringLiteral("🛰 Crew topology — not yet ported"));
    };
    return s;
}

}  // namespace odv
