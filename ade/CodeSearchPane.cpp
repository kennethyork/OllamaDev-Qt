#include "CodeSearchPane.h"

#include <QLabel>

namespace odv {

PaneSpec makeCodeSearchPaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("codesearch");
    s.title = QStringLiteral("🔎 Code search");
    s.group = QStringLiteral("Tools");
    s.singleton = true;
    s.factory = [](PaneHost&) -> QWidget* {
        // STUB placeholder — the real pane replaces this.
        return new QLabel(QStringLiteral("🔎 Code search — not yet ported"));
    };
    return s;
}

}  // namespace odv
