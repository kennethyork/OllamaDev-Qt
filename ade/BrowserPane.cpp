#include "BrowserPane.h"

#include <QLabel>

namespace odv {

PaneSpec makeBrowserPaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("browser");
    s.title = QStringLiteral("🌐 Browser");
    s.group = QStringLiteral("Views");
    s.singleton = true;
    s.factory = [](PaneHost&) -> QWidget* {
        // STUB placeholder — the real pane replaces this.
        return new QLabel(QStringLiteral("🌐 Browser — not yet ported"));
    };
    return s;
}

}  // namespace odv
