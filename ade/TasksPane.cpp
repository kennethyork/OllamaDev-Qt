#include "TasksPane.h"

#include <QLabel>

namespace odv {

PaneSpec makeTasksPaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("tasks");
    s.title = QStringLiteral("▦ Tasks");
    s.group = QStringLiteral("Views");
    s.singleton = true;
    s.factory = [](PaneHost&) -> QWidget* {
        // STUB placeholder — the real pane replaces this.
        return new QLabel(QStringLiteral("▦ Tasks — not yet ported"));
    };
    return s;
}

}  // namespace odv
