#include "Theme.h"

#include <QApplication>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QRegularExpression>
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

// The full App.THEMES table ported from app.js (~3461). `midnight` is first
// because it is the default there and here; the light themes (paper/porcelain/
// sandstone) are kept so the palette code is exercised on a light background too.
// The last colour is `canvas-grid-strong` — the emphasised grid dot the canvas
// paints; the CSS also carried a softer `canvas-grid`, but Canvas derives that
// from the accent, so only the strong value is needed here.
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
    {"carbon-orchard", "Carbon Orchard", "#111512", "#18201a", "#222c24", "#1d261f", "#edf4e9",
     "#a4ad9d", "#727c6b", "#334033", "#8ddf72", "#e4bd62", "#71d889", "#e4bd62", "#ef7168",
     "#0c100d", "#2c4f2d"},
    {"ink-terminal", "Ink Terminal", "#0e1013", "#151920", "#202633", "#1a202a", "#f0f3f7",
     "#9aa5b5", "#677281", "#303947", "#6dd3ff", "#f2a766", "#64d48c", "#f2c766", "#ff7770",
     "#090b0e", "#24465a"},
    {"warm-workshop", "Warm Workshop", "#151311", "#201c18", "#2b251f", "#24201b", "#f4efe8",
     "#b0a59a", "#7e746a", "#40372e", "#ffbe6a", "#63d2c6", "#7bd879", "#ffbe6a", "#ff786b",
     "#0f0d0b", "#574026"},
    {"arctic-graph", "Arctic Graph", "#10161b", "#172129", "#22303a", "#1b2831", "#eef7fb",
     "#9eb1bb", "#6d808a", "#334652", "#78d7ff", "#9ee6b8", "#78dea3", "#e8c468", "#ff7a7a",
     "#0a1014", "#24516a"},
    {"ember-lab", "Ember Lab", "#17100f", "#221615", "#301f1d", "#281a18", "#fbefea", "#b89b91",
     "#846b63", "#46312c", "#ff8a5c", "#7dd3c7", "#8bd77f", "#ffc76b", "#ff665f", "#100b0a",
     "#683b2d"},
    {"deep-sea", "Deep Sea", "#0b1418", "#111f25", "#1a2c34", "#16262d", "#e8f7f7", "#91abae",
     "#61797d", "#2b4248", "#35d5c6", "#76a9ff", "#63d98b", "#e2c76d", "#ff7470", "#071013",
     "#1e5a61"},
    {"aurora-forge", "Aurora Forge", "#11131a", "#191d28", "#242b39", "#1e2430", "#f0f5ff",
     "#a2adbf", "#707b8d", "#343e50", "#74f0b2", "#b28cff", "#74f0b2", "#f1c76e", "#ff7482",
     "#0b0e14", "#2b5c55"},
    {"copper-night", "Copper Night", "#15110f", "#211914", "#2b211a", "#261d17", "#f5efe7",
     "#ad9b8b", "#77685d", "#44362c", "#d99a63", "#72d0e2", "#8bd47a", "#e3bd68", "#f07867",
     "#0f0b09", "#5a3f2b"},
    {"mint-steel", "Mint Steel", "#111717", "#192322", "#243130", "#1e2a29", "#edf7f4", "#9aaca8",
     "#6b7d79", "#344643", "#7de3c3", "#f2b95f", "#75da8d", "#f2c866", "#f06f68", "#0b1111",
     "#2e5b50"},
    {"rose-circuit", "Rose Circuit", "#171216", "#221a21", "#30262e", "#281f27", "#faeff6",
     "#b59cab", "#806d79", "#463542", "#ff83b7", "#77d4ff", "#7bdb9b", "#f3c86a", "#ff7070",
     "#100c0f", "#66324e"},
    {"solar-console", "Solar Console", "#17140d", "#211d13", "#2d281a", "#272216", "#f7f1df",
     "#b5a985", "#80755c", "#453d28", "#ffd166", "#4dd4ac", "#82d977", "#ffd166", "#f7796d",
     "#100d08", "#5f4d22"},
    {"plum-oxide", "Plum Oxide", "#15121b", "#201a28", "#2b2436", "#251f2f", "#f4effa", "#a99ab8",
     "#786a89", "#40354f", "#c79bff", "#6fe0cc", "#7bdb8a", "#e7c36d", "#ff7280", "#0f0b15",
     "#533b72"},
    {"blueprint", "Blueprint", "#0d1420", "#141f2f", "#1e2e43", "#18263a", "#edf6ff", "#99aac0",
     "#65788f", "#2f435d", "#5fb3ff", "#ffd166", "#6fd28e", "#ffd166", "#ff7373", "#080e17",
     "#245e91"},
    {"matrix-calm", "Matrix Calm", "#0b120d", "#111b14", "#1a281e", "#16211a", "#eaf8ed", "#91a998",
     "#617568", "#2b3f31", "#62e27d", "#70d7e8", "#62e27d", "#d6c76f", "#f06e69", "#070d09",
     "#245c33"},
    {"slate-gold", "Slate Gold", "#121416", "#1a1e22", "#252b30", "#20252a", "#f1f2ee", "#a6aaa8",
     "#757b7b", "#394147", "#e8be62", "#68d0e2", "#77d584", "#e8be62", "#ef7468", "#0c0e10",
     "#5b4a27"},
    {"cyber-moss", "Cyber Moss", "#0f1410", "#171f18", "#202b22", "#1b261d", "#eef7e8", "#9cad96",
     "#6d7d67", "#334033", "#b7f264", "#5dd8ff", "#81d96f", "#e6c966", "#f17068", "#0a0f0b",
     "#405f29"},
    {"ruby-terminal", "Ruby Terminal", "#171011", "#221819", "#302223", "#281d1e", "#faeeee",
     "#b89c9e", "#806c6e", "#463334", "#ff6f91", "#70d6ff", "#7bd88f", "#f4c66b", "#ff6f6f",
     "#100a0b", "#67313b"},
    {"pacific-night", "Pacific Night", "#0e1318", "#151e27", "#202c38", "#1a2530", "#edf6fb",
     "#9babba", "#6b7c8a", "#334251", "#4fd1c5", "#7fb3ff", "#6dda8d", "#eac86b", "#f67272",
     "#080d12", "#23616a"},
    {"violet-mineral", "Violet Mineral", "#13131a", "#1c1d27", "#272a37", "#222430", "#f1f2fb",
     "#a2a7ba", "#73798d", "#393d4d", "#9f8cff", "#7de2b8", "#76d887", "#e7c86a", "#ff747c",
     "#0d0d13", "#443c70"},
    {"paper-light", "Paper Light", "#f4f1ea", "#ebe7dc", "#ded8ca", "#fffaf0", "#27231d", "#6f675b",
     "#948a7a", "#c9bfad", "#256f83", "#b35f2a", "#317a4c", "#a66d1e", "#b84b45", "#eee8db",
     "#9fb8bb"},
    {"porcelain-light", "Porcelain Light", "#f3f7f8", "#e8eef1", "#dce5e8", "#ffffff", "#1e2a2f",
     "#637279", "#89979d", "#c3d0d5", "#247ba0", "#d08b3e", "#2f855a", "#b7791f", "#c24a4a",
     "#edf3f5", "#a7c6d1"},
    {"sandstone-light", "Sandstone Light", "#f5efe5", "#ebe1d1", "#dfd2be", "#fff8ee", "#2c251d",
     "#71675b", "#978b7d", "#cabda9", "#9a5f2d", "#247b7b", "#3d7a45", "#a36f20", "#b84f45",
     "#eee2cf", "#b7925a"},
    {"terminal-green", "Terminal Green", "#07100a", "#0d1810", "#142419", "#101d14", "#dfffe7",
     "#86a78e", "#55715d", "#263a2b", "#50ff7a", "#ffd166", "#50ff7a", "#d8c76a", "#ff7070",
     "#040a06", "#276139"},
    {"operator-dark", "Operator Dark", "#0d0f12", "#14181d", "#1e242b", "#191e24", "#eff3f5",
     "#9aa4aa", "#69747b", "#303942", "#5eead4", "#f4b860", "#73d989", "#f4c66f", "#ff7474",
     "#080a0d", "#28615d"},
    {"cobalt-amber", "Cobalt Amber", "#0d1220", "#151d31", "#202b42", "#1a2439", "#eef4ff",
     "#9caac0", "#6d7a91", "#33435d", "#6aa9ff", "#ffbe5f", "#77d98a", "#ffbe5f", "#f77272",
     "#080d18", "#2a5793"},
    {"tidepool", "Tidepool", "#0d1515", "#142020", "#1f2f2f", "#192727", "#edf8f5", "#9bb0aa",
     "#6b807b", "#334845", "#69e2d0", "#b9d96b", "#7bd88b", "#e4c76a", "#f06f68", "#080f0f",
     "#2b625a"},
    {"hazel-code", "Hazel Code", "#121310", "#1b1d18", "#272a22", "#21241d", "#f1f4e9", "#a7ad9b",
     "#767c6b", "#3a4033", "#c9df70", "#75d6d0", "#82d978", "#dfbd66", "#ef7468", "#0c0d0a",
     "#52602e"},
    {"night-shift", "Night Shift", "#101216", "#181c23", "#232934", "#1d232c", "#f0f3f8", "#9da7b6",
     "#6c7786", "#343d4c", "#8ec5ff", "#f1a66a", "#77d98d", "#eec46d", "#ff7770", "#0a0c10",
     "#304d70"},
    {"orchid-noir", "Orchid Noir", "#141116", "#1f1923", "#2b2331", "#251e29", "#f7eff7", "#ad9bad",
     "#7b6c7c", "#403544", "#e48cff", "#6ed9c2", "#78d889", "#e7c56d", "#ff737a", "#0e0a10",
     "#5b3b65"},
    {"steel-rose", "Steel Rose", "#121416", "#1b1f23", "#262c31", "#20262a", "#f2f2f0", "#a8aaa7",
     "#777c7b", "#3b4247", "#f08aa6", "#70d6d0", "#78d68d", "#e8c66d", "#f06d6d", "#0c0e10",
     "#5d3b47"},
    {"lime-espresso", "Lime Espresso", "#13110d", "#1d1912", "#282219", "#231e16", "#f3efe4",
     "#aaa08d", "#776d5e", "#3f372b", "#b9e769", "#64d5d2", "#83da79", "#dfbc67", "#ef7168",
     "#0d0b08", "#4e5c2d"},
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

// ---- custom themes ----------------------------------------------------------
// User themes from the editor live in QSettings as one JSON object, id -> {name,
// <role>: <hex>, …} — the desktop's stand-in for the PHP app's localStorage
// ade.customThemes. They are additive: built-ins are never touched.
constexpr char kCustomKey[] = "ade/customThemes";

const char* const kRoleKeys[] = {"bg",     "bg2",    "bg3",  "elev",   "fg",
                                 "dim",    "faint",  "border", "accent", "accent2",
                                 "ok",     "warn",   "err",  "canvas", "canvasGrid"};

QJsonObject customObj() {
    QSettings s;
    return QJsonDocument::fromJson(s.value(kCustomKey).toString().toUtf8()).object();
}

void writeCustomObj(const QJsonObject& o) {
    QSettings s;
    s.setValue(kCustomKey, QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
}

Theme::Colors colorsFromJson(const QJsonObject& o) {
    Theme::Colors c;
    QColor* fields[] = {&c.bg,     &c.bg2,   &c.bg3,    &c.elev,   &c.fg,
                        &c.dim,    &c.faint, &c.border, &c.accent, &c.accent2,
                        &c.ok,     &c.warn,  &c.err,    &c.canvas, &c.canvasGrid};
    for (int i = 0; i < 15; ++i) *fields[i] = QColor(o.value(QLatin1String(kRoleKeys[i])).toString());
    return c;
}

QJsonObject colorsToJson(const Theme::Colors& c) {
    const QColor fields[] = {c.bg,  c.bg2,   c.bg3,    c.elev,   c.fg,
                             c.dim, c.faint, c.border, c.accent, c.accent2,
                             c.ok,  c.warn,  c.err,    c.canvas, c.canvasGrid};
    QJsonObject o;
    for (int i = 0; i < 15; ++i) o.insert(QLatin1String(kRoleKeys[i]), fields[i].name());
    return o;
}

}  // namespace

QStringList Theme::names() {
    QStringList out;
    for (const auto& e : kThemes) out << QLatin1String(e.id);
    // Custom themes follow the built-ins, sorted, so the editor's saves appear in
    // the same dropdown the built-ins do.
    QStringList custom = customObj().keys();
    custom.sort();
    out += custom;
    return out;
}

QString Theme::label(const QString& name) {
    const QJsonObject cu = customObj();
    if (cu.contains(name)) return cu.value(name).toObject().value("name").toString(name);
    return QLatin1String(find(name)->label);
}

bool Theme::isCustom(const QString& id) { return customObj().contains(id); }

QString Theme::slug(const QString& label) {
    QString s = label.toLower();
    s.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    s.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    return QStringLiteral("custom-") + s.left(40);
}

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
    const QJsonObject cu = customObj();
    if (cu.contains(name)) return colorsFromJson(cu.value(name).toObject());
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
    setCurrent(name);
    applyColors(app, colors(name));
}

QString Theme::saveCustom(const QString& label, const Colors& c) {
    const QString id = slug(label);
    QJsonObject cu = customObj();
    QJsonObject entry = colorsToJson(c);
    entry.insert("name", label.trimmed().isEmpty() ? QStringLiteral("Custom theme") : label.trimmed());
    cu.insert(id, entry);
    writeCustomObj(cu);
    return id;
}

void Theme::applyColors(QApplication* app, const Colors& c) {
    if (!app) return;

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
