#pragma once

class QWidget;

namespace odv {

// The theme editor — a port of the PHP app's "theme engine" (app.js ~3540):
// one colour field per role, a live preview that repaints the app as you type,
// and Save, which persists an editable custom theme (Theme::saveCustom) and
// applies it. open() is modal against `parent` (the main window).
class ThemeDialog {
public:
    static void open(QWidget* parent);
};

}  // namespace odv
