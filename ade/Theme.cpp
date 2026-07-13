#include "Theme.h"

#include <QApplication>
#include <QHash>
#include <QPalette>
#include <QSettings>

namespace odv {
namespace {

struct Entry {
    const char* id;
    const char* label;
    // Same 15 roles the CSS variables carried, same hex values.
    const char* bg;
    const char* bg2;
    const char* bg3;
    const char* elev;
    const char* fg;
    const char* dim;
    const char* faint;
    const char* border;
    const char* accent;
    const char* accent2;
    const char* ok;
    const char* warn;
    const char* err;
    const char* canvas;
    const char* canvasGrid;
};

// Twelve of the original thirty-six. `midnight` is first because it is the
// default there and here; `paper-light` is the one light theme, kept so the
// palette code is exercised on a light background too.
const Entry kThemes[] = {
    {"midnight", "Graphite Canvas", "#101214", "#171a1d", "#202428", "#1c2024", "#eef2f1", "#9aa4a2",
     "#687370", "#31383a", "#3bd4c7", "#f0b35b", "#57d77a", "#f0b35b", "#ff6b62", "#0b0d0e",
     "#214b49"},
    {"void", "Void", "#08080c", "#101018", "#181826", "#14141f", "#dcdce6", "#7c7c93", "#55556a",
     "#22222e", "#8b5cf6", "#22d3ee", "#34d399", "#fbbf24", "#fb7185", "#050507", "#30235a"},
    {"neon-tokyo", "Neon Tokyo", "#1a1b26", "#16161e", "#222234", "#1e1e2e", "#c0caf5", "#787c99",
     "#565f89", "#2a2e44", "#7aa2f7", "#bb9af7", "#9ece6a", "#e0af68", "#f7768e", "#11121a",
     "#384a70"},
    {"synthwave", "Synthwave", "#1a1530", "#241b3a", "#2e2348", "#281e42", "#f6f1ff", "#9d8bc9",
     "#6e5fa0", "#3a2d5c", "#ff5fd2", "#ff8b39", "#72f1b8", "#fede5d", "#fe4450", "#120d25",
     "#66366f"},
    {"dracula", "Dracula", "#282a36", "#21222c", "#343746", "#2b2d3a", "#f8f8f2", "#9aa0c0",
     "#6272a4", "#3b3d4d", "#bd93f9", "#8be9fd", "#50fa7b", "#f1fa8c", "#ff5555", "#1d1e27",
     "#554770"},
    {"mono", "Mono", "#0c0c0c", "#161616", "#1f1f1f", "#1a1a1a", "#e8e8e8", "#9a9a9a", "#6a6a6a",
     "#2c2c2c", "#cfcfcf", "#9a9a9a", "#bdbdbd", "#cccccc", "#e06666", "#080808", "#333333"},
    {"ink-terminal", "Ink Terminal", "#0e1013", "#151920", "#202633", "#1a202a", "#f0f3f7",
     "#9aa5b5", "#677281", "#303947", "#6dd3ff", "#f2a766", "#64d48c", "#f2c766", "#ff7770",
     "#090b0e", "#24465a"},
    {"deep-sea", "Deep Sea", "#0b1418", "#111f25", "#1a2c34", "#16262d", "#e8f7f7", "#91abae",
     "#61797d", "#2b4248", "#35d5c6", "#76a9ff", "#63d98b", "#e2c76d", "#ff7470", "#071013",
     "#1e5a61"},
    {"blueprint", "Blueprint", "#0d1420", "#141f2f", "#1e2e43", "#18263a", "#edf6ff", "#99aac0",
     "#65788f", "#2f435d", "#5fb3ff", "#ffd166", "#6fd28e", "#ffd166", "#ff7373", "#080e17",
     "#245e91"},
    {"terminal-green", "Terminal Green", "#07100a", "#0d1810", "#142419", "#101d14", "#dfffe7",
     "#86a78e", "#55715d", "#263a2b", "#50ff7a", "#ffd166", "#50ff7a", "#d8c76a", "#ff7070",
     "#040a06", "#276139"},
    {"operator-dark", "Operator Dark", "#0d0f12", "#14181d", "#1e242b", "#191e24", "#eff3f5",
     "#9aa4aa", "#69747b", "#303942", "#5eead4", "#f4b860", "#73d989", "#f4c66f", "#ff7474",
     "#080a0d", "#28615d"},
    {"paper-light", "Paper Light", "#f4f1ea", "#ebe7dc", "#ded8ca", "#fffaf0", "#27231d", "#6f675b",
     "#948a7a", "#c9bfad", "#256f83", "#b35f2a", "#317a4c", "#a66d1e", "#b84b45", "#eee8db",
     "#9fb8bb"},
};

const Entry* find(const QString& id) {
    for (const auto& e : kThemes)
        if (id == QLatin1String(e.id)) return &e;
    return &kThemes[0];
}

// A translucent tint of the accent, used for hovers. The CSS had --accent-soft
// derived the same way (rgba(accent, .16)).
QString soft(const QColor& c, double a) {
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red())
        .arg(c.green())
        .arg(c.blue())
        .arg(a, 0, 'f', 2);
}

}  // namespace

QStringList Theme::names() {
    QStringList out;
    for (const auto& e : kThemes) out << QLatin1String(e.id);
    return out;
}

QString Theme::label(const QString& name) { return QLatin1String(find(name)->label); }

QString Theme::current() {
    QSettings s;
    const QString id = s.value("ade/theme", "midnight").toString();
    return names().contains(id) ? id : QStringLiteral("midnight");
}

void Theme::setCurrent(const QString& name) {
    QSettings s;
    s.setValue("ade/theme", name);
}

Theme::Colors Theme::colors(const QString& name) {
    const Entry* e = find(name);
    Colors c;
    c.bg = QColor(e->bg);
    c.bg2 = QColor(e->bg2);
    c.bg3 = QColor(e->bg3);
    c.elev = QColor(e->elev);
    c.fg = QColor(e->fg);
    c.dim = QColor(e->dim);
    c.faint = QColor(e->faint);
    c.border = QColor(e->border);
    c.accent = QColor(e->accent);
    c.accent2 = QColor(e->accent2);
    c.ok = QColor(e->ok);
    c.warn = QColor(e->warn);
    c.err = QColor(e->err);
    c.canvas = QColor(e->canvas);
    c.canvasGrid = QColor(e->canvasGrid);
    return c;
}

Theme::Colors Theme::currentColors() { return colors(current()); }

void Theme::apply(QApplication* app, const QString& name) {
    if (!app) return;
    const Colors c = colors(name);
    setCurrent(name);

    QPalette p;
    p.setColor(QPalette::Window, c.bg);
    p.setColor(QPalette::WindowText, c.fg);
    p.setColor(QPalette::Base, c.bg2);
    p.setColor(QPalette::AlternateBase, c.bg3);
    p.setColor(QPalette::Text, c.fg);
    p.setColor(QPalette::PlaceholderText, c.faint);
    p.setColor(QPalette::Button, c.bg3);
    p.setColor(QPalette::ButtonText, c.fg);
    p.setColor(QPalette::BrightText, c.err);
    p.setColor(QPalette::Highlight, c.accent);
    // A light accent needs dark text on top of it; a dark accent needs light.
    p.setColor(QPalette::HighlightedText, c.accent.lightness() > 140 ? c.bg : c.fg);
    p.setColor(QPalette::ToolTipBase, c.elev);
    p.setColor(QPalette::ToolTipText, c.fg);
    p.setColor(QPalette::Link, c.accent);
    p.setColor(QPalette::Mid, c.border);
    p.setColor(QPalette::Dark, c.bg);
    p.setColor(QPalette::Disabled, QPalette::Text, c.faint);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, c.faint);
    p.setColor(QPalette::Disabled, QPalette::WindowText, c.faint);
    app->setPalette(p);

    // Widget-level styling the palette cannot express. Deliberately does NOT set
    // a background on bare QWidget: TerminalWidget and the canvas paint their own
    // and a blanket rule would fight them.
    const QString css =
        QStringLiteral(R"(
QMainWindow, QDialog, QMessageBox { background: %BG%; }
QLabel { color: %FG%; background: transparent; }
QToolTip { background: %ELEV%; color: %FG%; border: 1px solid %BORDER%; padding: 3px; }

QPushButton, QToolButton {
    background: %BG3%; color: %FG%; border: 1px solid %BORDER%;
    border-radius: 5px; padding: 4px 10px;
}
QPushButton:hover, QToolButton:hover { border-color: %ACCENT%; color: %ACCENT%; }
QPushButton:pressed, QToolButton:pressed { background: %SOFT%; }
QPushButton:disabled, QToolButton:disabled { color: %FAINT%; border-color: %BORDER%; }
QPushButton[cta="true"] { background: %ACCENT%; color: %BG%; border-color: %ACCENT%; font-weight: 600; }
QPushButton[danger="true"]:hover { border-color: %ERR%; color: %ERR%; }
QToolButton::menu-indicator { image: none; }

QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox, QAbstractSpinBox {
    background: %BG2%; color: %FG%; border: 1px solid %BORDER%;
    border-radius: 5px; padding: 3px 7px; selection-background-color: %ACCENT%;
}
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus { border-color: %ACCENT%; }

QComboBox {
    background: %BG3%; color: %FG%; border: 1px solid %BORDER%;
    border-radius: 5px; padding: 3px 22px 3px 8px;
}
QComboBox:hover { border-color: %ACCENT%; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox::down-arrow { image: none; border-left: 4px solid transparent;
    border-right: 4px solid transparent; border-top: 5px solid %DIM%; margin-right: 6px; }
QComboBox QAbstractItemView {
    background: %ELEV%; color: %FG%; border: 1px solid %BORDER%;
    selection-background-color: %ACCENT%; outline: 0;
}

QMenu { background: %ELEV%; color: %FG%; border: 1px solid %BORDER%; padding: 4px; }
QMenu::item { padding: 5px 22px 5px 12px; border-radius: 4px; }
QMenu::item:selected { background: %SOFT%; color: %ACCENT%; }
QMenu::separator { height: 1px; background: %BORDER%; margin: 4px 6px; }

QTabWidget::pane { border: 1px solid %BORDER%; background: %BG%; }
QTabBar::tab {
    background: %BG2%; color: %DIM%; border: 1px solid %BORDER%; border-bottom: none;
    padding: 4px 10px; margin-right: 2px;
}
QTabBar::tab:selected { background: %BG%; color: %FG%; border-top: 2px solid %ACCENT%; }
QTabBar::close-button { image: none; subcontrol-position: right; }

QTreeView, QListView {
    background: %BG2%; color: %FG%; border: none; outline: 0;
    selection-background-color: %SOFT%; selection-color: %ACCENT%;
}
QTreeView::item, QListView::item { padding: 2px 0; }
QTreeView::item:hover { background: %SOFT2%; }
QHeaderView::section {
    background: %BG3%; color: %DIM%; border: none; border-bottom: 1px solid %BORDER%; padding: 3px 6px;
}

QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
    background: %BORDER%; border-radius: 5px; min-height: 24px; min-width: 24px;
}
QScrollBar::handle:hover { background: %DIM%; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QSplitter::handle { background: %BORDER%; }
QScrollArea { border: none; background: transparent; }
)")
            .replace("%BG%", c.bg.name())
            .replace("%BG2%", c.bg2.name())
            .replace("%BG3%", c.bg3.name())
            .replace("%ELEV%", c.elev.name())
            .replace("%FG%", c.fg.name())
            .replace("%DIM%", c.dim.name())
            .replace("%FAINT%", c.faint.name())
            .replace("%BORDER%", c.border.name())
            .replace("%ACCENT%", c.accent.name())
            .replace("%ERR%", c.err.name())
            .replace("%SOFT2%", soft(c.accent, 0.07))
            .replace("%SOFT%", soft(c.accent, 0.16));
    app->setStyleSheet(css);
}

}  // namespace odv
