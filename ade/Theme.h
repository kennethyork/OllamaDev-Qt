#pragma once
#include <QColor>
#include <QFont>
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

    static QStringList names();                    // theme ids, in menu order (built-ins + custom)
    static QString label(const QString& name);     // human name for a theme id
    static QString current();
    static void setCurrent(const QString& name);   // persisted; does not re-apply
    static Colors colors(const QString& name);
    static Colors currentColors();
    static void apply(QApplication* app, const QString& name);

    // The same font, with an emoji font behind it in the family list.
    //
    // Qt does not fall back to a colour-bitmap emoji font on its own — least of all
    // for BOLD text, because Noto Color Emoji ships no bold face and the match
    // simply fails. So every emoji in the UI (pane titles, the Add menu, the
    // palette, and any emoji in a file you open) rendered as a tofu box. Qt walks a
    // family list per character, so the text stays in the requested font and only
    // the emoji come from the emoji font.
    //
    // Use this ANYWHERE a font is set by hand — a monospace editor font included,
    // which is how the tofu got into the editor after the app font was fixed.
    static QFont withEmoji(QFont f);

    // Live preview for the theme editor: paint an ARBITRARY palette without
    // persisting it as "the current theme". apply() is this plus setCurrent().
    static void applyColors(QApplication* app, const Colors& c);

    // Persist an editable custom theme (QSettings, the desktop's localStorage
    // analogue). Returns its generated id, which then shows up in names().
    static QString saveCustom(const QString& label, const Colors& c);
    static bool isCustom(const QString& id);
    static QString slug(const QString& label);
};

}  // namespace odv
