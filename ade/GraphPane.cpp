#include "GraphPane.h"

#include <QLabel>

namespace odv {

PaneSpec makeGraphPaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("graph");
    s.title = QStringLiteral("🕸 Memory graph");
    s.group = QStringLiteral("Views");
    s.singleton = true;
    s.factory = [](PaneHost&) -> QWidget* {
        // STUB placeholder — the real pane replaces this.
        return new QLabel(QStringLiteral("🕸 Memory graph — not yet ported"));
    };
    return s;
}

}  // namespace odv
