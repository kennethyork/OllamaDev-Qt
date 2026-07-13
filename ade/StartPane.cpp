#include "StartPane.h"

#include <QLabel>

namespace odv {

PaneSpec makeStartPaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("start");
    s.title = QStringLiteral("Start");
    s.group = QStringLiteral("Views");
    s.singleton = true;
    s.factory = [](PaneHost&) -> QWidget* {
        // STUB placeholder — the real pane replaces this.
        return new QLabel(QStringLiteral("Start — not yet ported"));
    };
    return s;
}

}  // namespace odv
