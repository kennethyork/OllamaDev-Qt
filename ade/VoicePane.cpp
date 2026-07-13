#include "VoicePane.h"

#include <QLabel>

namespace odv {

PaneSpec makeVoicePaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("voice");
    s.title = QStringLiteral("🎙 Voice");
    s.group = QStringLiteral("Tools");
    s.singleton = true;
    s.factory = [](PaneHost&) -> QWidget* {
        // STUB placeholder — the real pane replaces this.
        return new QLabel(QStringLiteral("🎙 Voice — not yet ported"));
    };
    return s;
}

}  // namespace odv
