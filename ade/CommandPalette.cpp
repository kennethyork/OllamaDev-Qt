#include "CommandPalette.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QShortcut>
#include <QVBoxLayout>
#include <functional>

#include "Canvas.h"
#include "PaneRegistry.h"
#include "Theme.h"

namespace odv {
namespace {

struct Command {
    QString title;
    QString hint;
    std::function<void()> run;
};

// The overlay is a CHILD of the main window (not a top-level dialog): it must
// cover exactly the window's client area and follow it on resize, which a child
// filtering its parent's resize events does for free. app.js did the same with a
// full-bleed #cmdOverlay div.
class PaletteOverlay : public QWidget {
public:
    PaletteOverlay(QWidget* win, PaneHost* host) : QWidget(win), win_(win), host_(host) {
        // Frameless click-catcher spanning the window; a translucent scrim is
        // painted in paintEvent so the canvas dims behind the panel.
        setGeometry(win_->rect());
        win_->installEventFilter(this);

        auto* col = new QVBoxLayout(this);
        col->setContentsMargins(0, 0, 0, 0);
        col->addStretch(1);

        // The panel sits centred horizontally near the top, like a real palette:
        // a row with stretch on either side of the fixed-width panel.
        auto* center = new QHBoxLayout;
        center->addStretch(1);
        panel_ = new QFrame(this);
        panel_->setObjectName("cmdPanel");
        panel_->setFixedWidth(560);
        center->addWidget(panel_);
        center->addStretch(1);
        col->addLayout(center);

        col->addStretch(4);

        auto* pv = new QVBoxLayout(panel_);
        pv->setContentsMargins(0, 0, 0, 0);
        pv->setSpacing(0);

        input_ = new QLineEdit(panel_);
        input_->setObjectName("cmdInput");
        input_->setPlaceholderText(tr("Type a command…  (↑↓ to move, Enter to run, Esc to close)"));
        input_->installEventFilter(this);
        pv->addWidget(input_);

        list_ = new QListWidget(panel_);
        list_->setObjectName("cmdList");
        list_->setFocusPolicy(Qt::NoFocus);  // keystrokes stay with the search field
        list_->setUniformItemSizes(true);
        pv->addWidget(list_);

        connect(input_, &QLineEdit::textChanged, this, [this] { refilter(); });
        connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem*) { runCurrent(); });
        connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem*) { runCurrent(); });

        buildCommands();
        applyStyle();
        hide();
    }

    void popup() {
        setGeometry(win_->rect());
        applyStyle();  // colours may have changed since last time (theme switch)
        input_->clear();
        refilter();
        show();
        raise();
        input_->setFocus();
    }

protected:
    // Follow the window's size and repaint the scrim when the theme changes.
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == win_ && e->type() == QEvent::Resize) setGeometry(win_->rect());
        if (o == input_ && e->type() == QEvent::KeyPress) {
            auto* k = static_cast<QKeyEvent*>(e);
            switch (k->key()) {
                case Qt::Key_Escape: hide(); return true;
                case Qt::Key_Down: move(+1); return true;
                case Qt::Key_Up: move(-1); return true;
                case Qt::Key_Return:
                case Qt::Key_Enter: runCurrent(); return true;
                default: break;
            }
        }
        return QWidget::eventFilter(o, e);
    }

    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Escape) hide();
        else QWidget::keyPressEvent(e);
    }

    // A click on the dimmed backdrop (never on the panel) dismisses, exactly like
    // clicking outside #cmdOverlay in the PHP app.
    void mousePressEvent(QMouseEvent* e) override {
        if (!panel_->geometry().contains(e->pos())) hide();
        else QWidget::mousePressEvent(e);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(0, 0, 0, 140));
    }

private:
    void buildCommands() {
        cmds_.clear();
        QWidget* win = win_;

        auto addPane = [win](const QString& kind) {
            // addPaneOfKind is a MainWindow slot; invoking it by name keeps the
            // palette decoupled from MainWindow's header — no new PaneHost surface.
            QMetaObject::invokeMethod(win, "addPaneOfKind", Qt::QueuedConnection,
                                      Q_ARG(QString, kind));
        };

        // The five views MainWindow builds in directly.
        cmds_.push_back({tr("New terminal"), tr("Open a shell terminal"),
                         [addPane] { addPane("terminal"); }});
        cmds_.push_back({tr("Board"), tr("Task and crew status"), [addPane] { addPane("board"); }});
        cmds_.push_back({tr("Editor"), tr("Open the editor window"), [addPane] { addPane("editor"); }});
        cmds_.push_back({tr("Files"), tr("Browse project files"), [addPane] { addPane("files"); }});
        cmds_.push_back({tr("Settings"), tr("Engine settings"), [addPane] { addPane("settings"); }});

        // Everything registered beyond the built-ins.
        for (const PaneSpec& spec : PaneRegistry::instance().all()) {
            const QString kind = spec.kind;
            cmds_.push_back({spec.title,
                             spec.group.isEmpty() ? tr("Open pane") : spec.group,
                             [addPane, kind] { addPane(kind); }});
        }

        // Canvas + theme.
        cmds_.push_back({tr("Center canvas"), tr("Fit every pane in view"), [win] {
                             if (auto* c = win->findChild<Canvas*>()) c->centerAll();
                         }});
        for (const QString& id : Theme::names()) {
            const QString label = Theme::label(id);
            cmds_.push_back({tr("Theme: %1").arg(label), tr("Switch the colour theme"), [win, id] {
                                 // onThemeChanged is a MainWindow slot — it applies the
                                 // palette AND refreshes the canvas/board in one call.
                                 QMetaObject::invokeMethod(win, "onThemeChanged",
                                                           Qt::QueuedConnection, Q_ARG(QString, id));
                             }});
        }
    }

    // Substring match over "title hint", case-insensitive — the same test app.js
    // used. Simple beats clever here: it is predictable while typing.
    void refilter() {
        const QString q = input_->text().trimmed().toLower();
        list_->clear();
        shown_.clear();
        for (int i = 0; i < cmds_.size(); ++i) {
            const Command& c = cmds_[i];
            if (!q.isEmpty() && !(c.title + ' ' + c.hint).toLower().contains(q)) continue;
            shown_.push_back(i);
            auto* it = new QListWidgetItem(list_);
            // The hint rides along on the item as a dim second column; a plain
            // "title — hint" keeps one delegate and still reads clearly.
            it->setText(c.hint.isEmpty() ? c.title
                                         : c.title + QStringLiteral("    —  ") + c.hint);
        }
        if (list_->count()) list_->setCurrentRow(0);
    }

    void move(int delta) {
        const int n = list_->count();
        if (n == 0) return;
        int row = list_->currentRow() + delta;
        row = qBound(0, row, n - 1);
        list_->setCurrentRow(row);
    }

    void runCurrent() {
        const int row = list_->currentRow();
        if (row < 0 || row >= shown_.size()) return;
        const int idx = shown_[row];
        hide();
        if (idx >= 0 && idx < cmds_.size() && cmds_[idx].run) cmds_[idx].run();
    }

    void applyStyle() {
        const Theme::Colors c = Theme::currentColors();
        panel_->setStyleSheet(
            QStringLiteral(R"(
#cmdPanel { background: %ELEV%; border: 1px solid %BORDER%; border-radius: 10px; }
#cmdInput {
    background: transparent; color: %FG%; border: none;
    border-bottom: 1px solid %BORDER%; border-top-left-radius: 10px;
    border-top-right-radius: 10px; padding: 12px 14px; font-size: 15px;
}
#cmdList {
    background: transparent; color: %FG%; border: none; outline: 0;
    padding: 6px; max-height: 360px;
}
#cmdList::item { padding: 7px 10px; border-radius: 6px; }
#cmdList::item:selected { background: %SOFT%; color: %ACCENT%; }
)")
                .replace("%ELEV%", c.elev.name())
                .replace("%BORDER%", c.border.name())
                .replace("%FG%", c.fg.name())
                .replace("%ACCENT%", c.accent.name())
                .replace("%SOFT%", QStringLiteral("rgba(%1,%2,%3,0.18)")
                                       .arg(c.accent.red())
                                       .arg(c.accent.green())
                                       .arg(c.accent.blue())));
    }

    QWidget* win_ = nullptr;
    PaneHost* host_ = nullptr;
    QFrame* panel_ = nullptr;
    QLineEdit* input_ = nullptr;
    QListWidget* list_ = nullptr;
    QVector<Command> cmds_;
    QVector<int> shown_;  // command index for each visible row
};

}  // namespace

void CommandPalette::install(PaneHost& host) {
    QWidget* win = host.window();
    if (!win) return;

    // The overlay is created once and reused; it lives as a child of the window.
    auto* overlay = new PaletteOverlay(win, &host);

    auto* sc = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_K), win);
    sc->setContext(Qt::ApplicationShortcut);
    QObject::connect(sc, &QShortcut::activated, overlay, [overlay] { overlay->popup(); });
}

}  // namespace odv
