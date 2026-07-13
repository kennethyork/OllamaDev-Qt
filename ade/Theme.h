#pragma once
#include <QColor>
#include <QString>
#include <QStringList>

class QApplication;

namespace odv {

// The theme system is a straight port of App.THEMES in the PHP app's app.js: a
// flat table of colour roles, applied globally. There it was CSS custom
// properties on <body>; here it is a QPalette plus one application stylesheet,
// because Qt has no cascade and a palette alone cannot reach the details
// (borders, headers, tab bars) the original styled by hand.
class Theme {
public:
    struct Colors {
        QColor bg, bg2, bg3, elev, fg, dim, faint, border, accent, accent2, ok, warn, err, canvas,
            canvasGrid;
    };

    static QStringList names();                    // theme ids, in menu order
    static QString label(const QString& name);     // human name for a theme id
    static QString current();
    static void setCurrent(const QString& name);   // persisted; does not re-apply
    static Colors colors(const QString& name);
    static Colors currentColors();
    static void apply(QApplication* app, const QString& name);
};

}  // namespace odv
