// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "MainWindow.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QShortcut>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QSettings>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "Backend.h"
#include "BoardPane.h"
#include "Canvas.h"
#include "CommandPalette.h"
#include "PaneRegistry.h"
#include "Config.h"
#include "Crew.h"
#include "EditorPane.h"
#include "FilesPane.h"
#include "ManageDialogs.h"
#include "Mcp.h"
#include "OllamaBackend.h"
#include "Pane.h"
#include "TerminalWidget.h"
#include "Theme.h"
#include "Workspaces.h"
#include "ThemeDialog.h"
#include "Tools.h"

namespace odv {
namespace {

// Pane ids for the singleton views. Deliberately the SAME strings the PHP app
// used (`'__pop_' + view + '__'`) so a workspaces.json written by either app
// round-trips through the other.
QString viewPaneId(const QString& view) { return QStringLiteral("__pop_%1__").arg(view); }

// The four views built into MainWindow (their own ensure* helpers, not the
// registry). They stay in the PHP-compatible `panes` map; every other canvas
// pane is a registry pane persisted in `extraPanes`.
bool isBuiltinView(const QString& kind) {
    return kind == QLatin1String("board") || kind == QLatin1String("editor") ||
           kind == QLatin1String("files") || kind == QLatin1String("settings");
}

// The registry kind behind a pane id: "__pop_<kind>__" for a singleton, or
// "<kind>_<seq>" for a stacked one (kinds use '-', never '_', so the last '_'
// splits kind from sequence).
QString paneKindFromId(const QString& id) {
    if (id.startsWith(QLatin1String("__pop_")) && id.endsWith(QLatin1String("__")))
        return id.mid(6, id.size() - 8);
    const int u = id.lastIndexOf(QLatin1Char('_'));
    return u > 0 ? id.left(u) : id;
}

QRectF geomFromJson(const QJsonObject& o) {
    if (!o.contains("w")) return QRectF();
    return QRectF(o.value("x").toDouble(), o.value("y").toDouble(), o.value("w").toDouble(),
                  o.value("h").toDouble());
}

QJsonObject geomToJson(const QRectF& r) {
    return QJsonObject{{"x", r.x()}, {"y", r.y()}, {"w", r.width()}, {"h", r.height()}};
}

QString userShell() { return qEnvironmentVariable("SHELL", QStringLiteral("/bin/bash")); }

// The C++ ollamadev this app must drive — NOT whatever `ollamadev` resolves to on
// the user's PATH, which may well be an old PHP build in ~/.local/bin. Resolve
// OUR binary: an explicit override, then next to the app (installed side-by-side,
// or ../cli in the build tree), then a bundled bin/, and only as a last resort
// the bare name. Returned shell-quoted so a path with spaces is safe.
QString odvCli() {
    static const QString path = [] {
        const QByteArray env = qgetenv("OLLAMADEV_BINARY");
        if (!env.isEmpty() && QFileInfo(QString::fromLocal8Bit(env)).isExecutable())
            return QString::fromLocal8Bit(env);
        const QString dir = QCoreApplication::applicationDirPath();
        for (const QString& cand : {dir + "/ollamadev", dir + "/../cli/ollamadev",
                                    dir + "/../bin/ollamadev", dir + "/bin/ollamadev"}) {
            const QFileInfo fi(cand);
            if (fi.isFile() && fi.isExecutable()) return fi.absoluteFilePath();
        }
        return QStringLiteral("ollamadev");  // fall back to PATH
    }();
    return path.contains(QLatin1Char(' ')) ? QStringLiteral("'%1'").arg(path) : path;
}

// Rewrite a leading `ollamadev` token to OUR resolved binary; other CLIs
// (claude/codex/…) are external and stay as-is for PATH to resolve.
QString withOdvCli(const QString& command) {
    if (command == QLatin1String("ollamadev")) return odvCli();
    if (command.startsWith(QLatin1String("ollamadev ")))
        return odvCli() + command.mid(QStringLiteral("ollamadev").size());
    return command;
}

}  // namespace

// The folder to open at launch. An explicit path (command-line / .desktop `%F`)
// wins; otherwise the MOST RECENTLY USED workspace — a desktop app is normally
// launched from a menu, whose cwd is only ever $HOME, so the launch directory is
// not a reliable signal for which project you want. "Reopen what I last worked in"
// is. The launch cwd is the last resort, when there are no workspaces at all.
static QString startupProject(const QString& explicitPath) {
    if (!explicitPath.isEmpty()) {
        const QString abs = QDir(explicitPath).absolutePath();
        if (QFileInfo(abs).isDir()) return abs;
    }
    QString best;
    QDateTime bestWhen;
    for (const Workspace& w : Workspaces::all()) {
        if (!QFileInfo(w.path).isDir()) continue;  // a bookmarked folder since deleted
        const QDateTime when = QDateTime::fromString(w.lastOpened, Qt::ISODate);
        if (!when.isValid()) {
            if (best.isEmpty()) best = w.path;  // only as a last resort
            continue;
        }
        if (best.isEmpty() || !bestWhen.isValid() || when > bestWhen) {
            best = w.path;
            bestWhen = when;
        }
    }
    if (!best.isEmpty()) return best;
    return QDir::currentPath();
}

MainWindow::MainWindow(const QString& startupPath, QWidget* parent) : QMainWindow(parent) {
    project_ = startupProject(startupPath);
    wsId_ = workspaceId(project_);

    // The window may have been launched from a menu (cwd == $HOME) but is opening a
    // different project. Re-root everything now, exactly as openProject() does, or
    // the file tree, the crew, and every tool would quietly operate in $HOME.
    QDir::setCurrent(project_);
    Tools::setThreadRoot(project_);
    Config::load();

    setWindowTitle(QStringLiteral("OllamaDev ADE — %1").arg(QFileInfo(project_).fileName()));
    resize(1440, 900);

    registerExtraPanes();  // populate the registry before the Add menu is built

    canvas_ = new Canvas(this);
    connect(canvas_, &Canvas::paneClosed, this, &MainWindow::onPaneClosed);
    connect(canvas_, &Canvas::contextMenuRequestedAt, this, &MainWindow::onCanvasContextMenu);

    // Central column: a slim crew-resume banner (hidden until offered) stacked above
    // the canvas, so the banner can appear without floating over or shrinking panes.
    auto* central = new QWidget(this);
    auto* col = new QVBoxLayout(central);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);
    col->addWidget(buildCrewBanner());
    col->addWidget(canvas_, 1);
    setCentralWidget(central);

    buildTopBar();
    statusBar()->showMessage(tr("ready"));

    net_ = new QNetworkAccessManager(this);
    ping_ = new QTimer(this);
    ping_->setInterval(5000);
    connect(ping_, &QTimer::timeout, this, &MainWindow::pingOllama);
    ping_->start();
    pingOllama();

    loadSession();
    offerCrewResumeIfAny();

    autosave_ = new QTimer(this);
    autosave_->setInterval(4000);
    connect(autosave_, &QTimer::timeout, this, &MainWindow::autosave);
    autosave_->start();

    // Ctrl-K command palette. install() owns the shortcut and overlay; it reaches
    // pane-open and theme actions through this window's slots, so no new PaneHost
    // surface is needed.
    CommandPalette::install(*this);
}

MainWindow::~MainWindow() = default;

// ---- top bar ---------------------------------------------------------------

void MainWindow::buildTopBar() {
    auto* bar = new QWidget(this);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(10, 6, 10, 6);
    row->setSpacing(8);

    conn_ = new QLabel(QStringLiteral("●"), bar);
    conn_->setToolTip(tr("Ollama connection"));
    row->addWidget(conn_);

    auto* connText = new QLabel(tr("Ollama"), bar);
    connText->setObjectName("connText");
    row->addWidget(connText);

    models_ = new QComboBox(bar);
    models_->setMinimumWidth(190);
    models_->setToolTip(tr("Model"));
    row->addWidget(models_);
    connect(models_, &QComboBox::currentTextChanged, this, [](const QString& m) {
        if (!m.isEmpty()) Config::setPref(QStringLiteral("ollama.defaultModel"), m);
    });

    perms_ = new QComboBox(bar);
    perms_->addItem(tr("Ask · confirm each change"), QStringLiteral("ask"));
    perms_->addItem(tr("Auto · run tools without asking"), QStringLiteral("auto"));
    perms_->addItem(tr("Read-only · no changes"), QStringLiteral("readonly"));
    perms_->setToolTip(tr("How the agent runs tools"));
    {
        const QString saved = Config::str(QStringLiteral("permission.mode"), QStringLiteral("ask"));
        const int i = perms_->findData(saved);
        perms_->setCurrentIndex(i >= 0 ? i : 0);
        Permission::setMode(Permission::modeFromName(saved));
    }
    connect(perms_, &QComboBox::currentIndexChanged, this, [this](int i) {
        const QString mode = perms_->itemData(i).toString();
        Config::setPref(QStringLiteral("permission.mode"), mode);
        Permission::setMode(Permission::modeFromName(mode));
        status(tr("permission mode: %1").arg(mode));
    });
    row->addWidget(perms_);

    auto* add = new QToolButton(bar);
    add->setText(tr("＋ Add ▾"));
    add->setProperty("cta", true);
    add->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(add);
    buildAddMenu(menu);
    add->setMenu(menu);
    row->addWidget(add);

    // Management dialogs + theme editor (ported from the PHP app's manage menu).
    auto* manage = new QToolButton(bar);
    manage->setText(tr("Manage ▾"));
    manage->setPopupMode(QToolButton::InstantPopup);
    auto* mmenu = new QMenu(manage);
    connect(mmenu->addAction(tr("Launch Crew…")), &QAction::triggered, this,
            [this] { ManageDialogs::openCrewLaunch(*this); });
    connect(mmenu->addAction(tr("Review changes…")), &QAction::triggered, this,
            [this] { ManageDialogs::openReview(*this); });
    connect(mmenu->addAction(tr("Workspaces…")), &QAction::triggered, this, [this] {
        ManageDialogs::openWorkspaces(*this, [this](const QString& p) { openProject(p); });
    });
    mmenu->addSeparator();
    connect(mmenu->addAction(tr("Crew roles…")), &QAction::triggered, this,
            [this] { ManageDialogs::openRoles(*this); });
    connect(mmenu->addAction(tr("Skills…")), &QAction::triggered, this,
            [this] { ManageDialogs::openSkills(*this); });
    connect(mmenu->addAction(tr("Hooks…")), &QAction::triggered, this,
            [this] { ManageDialogs::openHooks(*this); });
    mmenu->addSeparator();
    // The overview map. Remembered, because it is a matter of taste: on a canvas
    // with three panes it is clutter, and on one with fifteen it is the only way to
    // know where anything is.
    auto* mapAct = mmenu->addAction(tr("Overview map"));
    mapAct->setCheckable(true);
    {
        QSettings s;
        const bool on = s.value(QStringLiteral("canvas/minimap"), true).toBool();
        mapAct->setChecked(on);
        canvas_->setMinimapVisible(on);
    }
    connect(mapAct, &QAction::toggled, this, [this](bool on) {
        canvas_->setMinimapVisible(on);
        QSettings().setValue(QStringLiteral("canvas/minimap"), on);
    });
    connect(mmenu->addAction(tr("Theme editor…")), &QAction::triggered, this,
            [this] { ThemeDialog::open(this); });
    manage->setMenu(mmenu);
    row->addWidget(manage);

    row->addStretch(1);

    // The project switcher. This used to be a dead QLabel showing the directory the
    // app happened to be launched from — and there was NO way to open another one.
    // You had to quit, cd, and relaunch.
    projectBtn_ = new QToolButton(bar);
    projectBtn_->setText(QFileInfo(project_).fileName());
    projectBtn_->setToolTip(project_);
    projectBtn_->setPopupMode(QToolButton::InstantPopup);
    projectBtn_->setStyleSheet(
        QStringLiteral("QToolButton{color:%1;border:1px solid %2;border-radius:6px;padding:4px 9px}"
                       "QToolButton:hover{color:%3;border-color:%3}")
            .arg(Theme::currentColors().dim.name(), Theme::currentColors().border.name(),
                 Theme::currentColors().accent.name()));
    auto* pmenu = new QMenu(projectBtn_);
    connect(pmenu, &QMenu::aboutToShow, this, [this, pmenu] { buildProjectMenu(pmenu); });
    projectBtn_->setMenu(pmenu);
    row->addWidget(projectBtn_);

    auto* open = new QShortcut(QKeySequence::Open, this);  // Ctrl+O
    connect(open, &QShortcut::activated, this, [this] { chooseProject(); });

    themes_ = new QComboBox(bar);
    for (const QString& id : Theme::names()) themes_->addItem(Theme::label(id), id);
    themes_->setCurrentIndex(qMax(0, themes_->findData(Theme::current())));
    themes_->setToolTip(tr("Theme"));
    connect(themes_, &QComboBox::currentIndexChanged, this,
            [this](int i) { onThemeChanged(themes_->itemData(i).toString()); });
    row->addWidget(themes_);

    setMenuWidget(bar);
}

void MainWindow::buildAddMenu(QMenu* menu) {
    const QVector<QPair<QString, QString>> kinds{{QStringLiteral("terminal"), tr("Terminal")},
                                                 {QStringLiteral("board"), tr("Board")},
                                                 {QStringLiteral("editor"), tr("Editor")},
                                                 {QStringLiteral("files"), tr("Files")},
                                                 {QStringLiteral("settings"), tr("Settings")}};
    for (const auto& k : kinds) {
        QAction* a = menu->addAction(k.second);
        const QString kind = k.first;
        connect(a, &QAction::triggered, this, [this, kind] { addPaneOfKind(kind); });
    }

    // A terminal that opens straight into a CLI. ollamadev (our own REPL) leads;
    // then every OTHER coding CLI actually installed on this machine. The list is
    // gated on availability the same way `ollamadev backends` is, so we never
    // offer a terminal for a CLI that is not there.
    QMenu* cli = menu->addMenu(tr("CLI terminal"));
    connect(cli->addAction(tr("ollamadev — interactive")), &QAction::triggered, this,
            [this] { addCliTerminal(QStringLiteral("ollamadev")); });
    cli->addSeparator();
    for (const QString& id : Backends::availableIds()) {
        if (id == "ollama") continue;  // that is the HTTP backend, not a CLI to open
        connect(cli->addAction(Backends::labelFor(id)), &QAction::triggered, this,
                [this, id] { addCliTerminal(id); });
    }

    // Everything registered beyond the built-ins, grouped by section.
    QString lastGroup;
    for (const auto& spec : PaneRegistry::instance().all()) {
        if (spec.group != lastGroup) {
            menu->addSeparator();
            lastGroup = spec.group;
        }
        const QString kind = spec.kind;
        connect(menu->addAction(spec.title), &QAction::triggered, this,
                [this, kind] { addPaneOfKind(kind); });
    }
    menu->addSeparator();
    connect(menu->addAction(tr("Remote VPS (MCP)…")), &QAction::triggered, this,
            [this] { addVpsMcp(); });
    menu->addSeparator();
    QAction* center = menu->addAction(tr("Center canvas"));
    connect(center, &QAction::triggered, canvas_, &Canvas::centerAll);
}

// Register a remote MCP server (typically one running on a VPS) from the GUI, so
// its tools join the crew's toolset exactly like a local one. Writes an http
// entry to config.json via Mcp::addHttpServer; the optional token becomes a
// Bearer header for a server behind an auth proxy.
void MainWindow::addVpsMcp() {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Add remote VPS MCP server"));
    auto* form = new QFormLayout(&dlg);

    auto* name = new QLineEdit(&dlg);
    name->setPlaceholderText(tr("my-vps"));
    form->addRow(tr("Name"), name);

    auto* url = new QLineEdit(&dlg);
    url->setPlaceholderText(QStringLiteral("https://your-vps:8000/mcp"));
    form->addRow(tr("Server URL"), url);

    auto* token = new QLineEdit(&dlg);
    token->setEchoMode(QLineEdit::Password);
    token->setPlaceholderText(tr("only if the server needs auth"));
    form->addRow(tr("Bearer token"), token);

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    QString err;
    if (Mcp::addHttpServer(name->text().trimmed(), url->text().trimmed(),
                           token->text().trimmed(), &err)) {
        status(tr("added MCP server '%1' — it will load on the next crew launch")
                   .arg(name->text().trimmed()));
    } else {
        QMessageBox::warning(this, tr("Could not add server"), err);
    }
}

// Right-clicking empty canvas offers the same menu — and drops the new pane at
// the point that was clicked, like showCanvasMenu()/addPane(kind, wx, wy).
void MainWindow::onCanvasContextMenu(const QPoint& globalPos, const QPointF& worldPos) {
    QMenu menu(this);
    const QVector<QPair<QString, QString>> kinds{{QStringLiteral("terminal"), tr("Terminal")},
                                                 {QStringLiteral("board"), tr("Board")},
                                                 {QStringLiteral("editor"), tr("Editor")},
                                                 {QStringLiteral("files"), tr("Files")},
                                                 {QStringLiteral("settings"), tr("Settings")}};
    QHash<QAction*, QString> map;
    for (const auto& k : kinds) map.insert(menu.addAction(k.second), k.first);
    menu.addSeparator();
    QAction* center = menu.addAction(tr("Center canvas"));

    QAction* hit = menu.exec(globalPos);
    if (!hit) return;
    if (hit == center) {
        canvas_->centerAll();
        return;
    }
    const QString kind = map.value(hit);
    if (kind.isEmpty()) return;

    // Spawn AT the click, sized like a default pane.
    const QSizeF sz(kind == "files" ? 320 : 560, kind == "board" ? 460 : 380);
    const QRectF at(worldPos, sz);
    if (kind == "terminal")
        addTerminal(QStringLiteral("term_%1").arg(++termSeq_), project_, QString(), at);
    else if (kind == "board") ensureBoard(at);
    else if (kind == "editor") ensureEditor(at);
    else if (kind == "files") ensureFiles(at);
    else if (kind == "settings") ensureSettings(at);
}

void MainWindow::addPaneOfKind(const QString& kind) {
    if (kind == "terminal")
        addTerminal(QStringLiteral("term_%1").arg(++termSeq_), project_, QString(), QRectF());
    else if (kind == "board") ensureBoard();
    else if (kind == "editor") ensureEditor();
    else if (kind == "files") ensureFiles();
    else if (kind == "settings") ensureSettings();
    else if (const PaneSpec* spec = PaneRegistry::instance().find(kind)) {
        // A registered pane. Singletons re-raise; the rest stack up.
        const QString id = spec->singleton ? viewPaneId(kind)
                                            : QStringLiteral("%1_%2").arg(kind).arg(++termSeq_);
        addRegistryPane(*spec, id, QRectF());
    }
}

Pane* MainWindow::addRegistryPane(const PaneSpec& spec, const QString& id, const QRectF& geom) {
    // An id already on the canvas (a singleton, or a restore that ran twice) is
    // re-raised rather than duplicated.
    if (Pane* p = canvas_->pane(id)) {
        canvas_->raisePane(p);
        return p;
    }
    QWidget* w = spec.factory(*this);
    if (!w) return nullptr;
    return canvas_->addPane(id, spec.title, w, geom);
}

// ---- panes -----------------------------------------------------------------

Pane* MainWindow::addTerminal(const QString& id, const QString& cwd, const QString& replay,
                              const QRectF& geom) {
    auto* term = new TerminalWidget;
    // Carried on the widget so captureState() can rebuild this terminal without a
    // parallel bookkeeping map that could drift out of sync with the canvas.
    term->setProperty("odvCwd", cwd.isEmpty() ? project_ : cwd);
    term->setProperty("odvKind", QStringLiteral("shell"));

    Pane* p = canvas_->addPane(id, tr("Terminal"), term, geom);
    connect(term, &TerminalWidget::titleChanged, p, [p](const QString& t) {
        if (!t.isEmpty()) p->setTitle(t);
    });
    connect(term, &TerminalWidget::exited, p,
            [p](int code) { p->setTitle(tr("Terminal — exited (%1)").arg(code)); });

    term->start(userShell(), {}, cwd.isEmpty() ? project_ : cwd);
    // Scrollback from a previous session is painted back in, read-only, exactly
    // like replaySnap in the PHP terminal.
    if (!replay.isEmpty()) term->replay(replay);

    const int seq = id.mid(5).toInt();
    if (seq > termSeq_) termSeq_ = seq;
    return p;
}

// A terminal that opens straight into a CLI. `cliId` is a backend id ("ollamadev"
// for our own REPL, or claude/codex/gemini/...). We launch it by SENDING the
// command into the interactive shell rather than exec-ing the binary directly:
// the shell has already sourced the user's rc files, so nvm / ~/.local/bin are on
// PATH there even when the GUI app's own environment (launched from a desktop
// menu) never saw them. When the CLI exits you drop back to a usable shell.
Pane* MainWindow::addCliTerminal(const QString& cliId) {
    Pane* p = addTerminal(QStringLiteral("term_%1").arg(++termSeq_), project_, QString(), QRectF());
    if (!p) return nullptr;
    auto* term = qobject_cast<TerminalWidget*>(p->content());
    if (!term) return p;
    term->setProperty("odvKind", cliId);
    p->setTitle(cliId);
    // For our own REPL, launch the C++ binary this app ships — not a stale PHP
    // `ollamadev` on PATH. Other CLIs are external and launch by their own name.
    // `clear;` wipes the shell greeting and the echoed (long, ugly, wrapped) launch
    // command so the pane opens straight onto the CLI, not the path we typed to run it.
    term->sendText(QStringLiteral("clear; ") + withOdvCli(cliId) + QLatin1Char('\n'));
    return p;
}

Pane* MainWindow::ensureBoard(const QRectF& geom) {
    if (Pane* p = canvas_->pane(viewPaneId("board"))) {
        canvas_->raisePane(p);
        return p;
    }
    board_ = new BoardPane;
    connect(board_, &BoardPane::statusMessage, this, &MainWindow::status);
    return canvas_->addPane(viewPaneId("board"), tr("Board"), board_,
                            geom.isValid() ? geom : QRectF());
}

Pane* MainWindow::ensureEditor(const QRectF& geom) {
    if (Pane* p = canvas_->pane(viewPaneId("editor"))) {
        canvas_->raisePane(p);
        return p;
    }
    editor_ = new EditorPane;
    connect(editor_, &EditorPane::statusMessage, this, &MainWindow::status);
    return canvas_->addPane(viewPaneId("editor"), tr("Editor"), editor_,
                            geom.isValid() ? geom : QRectF());
}

Pane* MainWindow::ensureFiles(const QRectF& geom) {
    if (Pane* p = canvas_->pane(viewPaneId("files"))) {
        canvas_->raisePane(p);
        return p;
    }
    files_ = new FilesPane(project_);
    connect(files_, &FilesPane::statusMessage, this, &MainWindow::status);
    connect(files_, &FilesPane::fileActivated, this, [this](const QString& path) {
        ensureEditor();
        editor_->openFile(path);
        canvas_->raisePane(canvas_->pane(viewPaneId("editor")));
    });
    return canvas_->addPane(viewPaneId("files"), tr("Files"), files_,
                            geom.isValid() ? geom : QRectF());
}

Pane* MainWindow::ensureSettings(const QRectF& geom) {
    if (Pane* p = canvas_->pane(viewPaneId("settings"))) {
        canvas_->raisePane(p);
        return p;
    }
    return canvas_->addPane(viewPaneId("settings"), tr("Settings"), buildSettingsWidget(),
                            geom.isValid() ? geom : QRectF());
}

// Engine settings only. They go to ade-prefs.json (Config::setPref), which the
// CLI overlays on load — config.json stays MCP-only, as in the PHP app.
QWidget* MainWindow::buildSettingsWidget() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(12, 12, 12, 12);
    auto* form = new QFormLayout;
    v->addLayout(form);

    auto* host = new QLineEdit(
        Config::str(QStringLiteral("ollama.host"), QStringLiteral("http://localhost:11434")), w);
    host->setPlaceholderText(QStringLiteral("http://localhost:11434"));
    form->addRow(tr("Ollama host"), host);

    // Bearer token for a remote host that sits behind an auth proxy — Ollama has
    // none of its own, so a public VPS is normally fronted by one. Blank == local,
    // no header sent. Masked because it is a credential.
    auto* token = new QLineEdit(Config::str(QStringLiteral("ollama.authToken")), w);
    token->setEchoMode(QLineEdit::Password);
    token->setPlaceholderText(tr("only needed for a secured remote host"));
    form->addRow(tr("Auth token"), token);

    auto* temp = new QDoubleSpinBox(w);
    temp->setRange(0.0, 2.0);
    temp->setSingleStep(0.1);
    temp->setValue(Config::number(QStringLiteral("agents.coder.temperature"), 0.7));
    form->addRow(tr("Temperature"), temp);

    auto* save = new QPushButton(tr("Save"), w);
    save->setProperty("cta", true);
    v->addWidget(save, 0, Qt::AlignLeft);
    v->addStretch(1);

    connect(save, &QPushButton::clicked, this, [this, host, token, temp] {
        Config::setPref(QStringLiteral("ollama.host"), host->text().trimmed());
        Config::setPref(QStringLiteral("ollama.authToken"), token->text().trimmed());
        Config::setPref(QStringLiteral("agents.coder.temperature"), temp->value());
        status(tr("settings saved to ade-prefs.json"));
        pingOllama();
    });
    return w;
}

// The crew-resume strip: a label plus Resume / Dismiss, hidden until an
// interrupted run for this project is found. Built once; offerCrewResumeIfAny()
// fills in the text and shows it.
QWidget* MainWindow::buildCrewBanner() {
    const Theme::Colors c = Theme::currentColors();
    crewBanner_ = new QFrame(this);
    crewBanner_->setStyleSheet(
        QStringLiteral("QFrame{background:%1;border-bottom:1px solid %2}")
            .arg(c.bg2.name(), c.border.name()));
    auto* row = new QHBoxLayout(crewBanner_);
    row->setContentsMargins(12, 6, 10, 6);
    row->setSpacing(8);

    crewBannerText_ = new QLabel(crewBanner_);
    crewBannerText_->setTextFormat(Qt::PlainText);
    row->addWidget(crewBannerText_, 1);

    auto* resume = new QPushButton(tr("Resume"), crewBanner_);
    resume->setProperty("cta", true);
    auto* dismiss = new QPushButton(tr("Dismiss"), crewBanner_);
    row->addWidget(resume);
    row->addWidget(dismiss);

    connect(resume, &QPushButton::clicked, this, [this] {
        // Open a CLI pane that finishes the run from plan.json — keeps what's done,
        // re-plans only the leftover work. Same path as `ollamadev crew resume <id>`.
        if (!crewResumeId_.isEmpty())
            runInTerminal(QStringLiteral("ollamadev crew resume ") + crewResumeId_);
        crewBanner_->hide();
    });
    connect(dismiss, &QPushButton::clicked, this, [this] { crewBanner_->hide(); });

    crewBanner_->hide();
    return crewBanner_;
}

// Offer to resume ONLY a run that was interrupted in THIS project. current.json
// still existing (with unfinished subtasks) is the signal an in-flight run was cut
// off — a clean finish leaves done == total. A completed or other-project run never
// shows a banner, so this can never nag about work that is already done.
void MainWindow::offerCrewResumeIfAny() {
    if (!crewBanner_) return;
    crewResumeId_.clear();
    crewBanner_->hide();

    const QString liveId = Crew::boardState().value(QStringLiteral("runId")).toString();
    if (liveId.isEmpty()) return;  // no in-flight run recorded

    for (const Crew::RunInfo& r : Crew::resumable()) {  // newest first
        if (r.runId != liveId) continue;                // only the in-flight run has live progress
        if (r.cwd != project_) return;                  // …and only if it belongs to this project
        if (r.total > 0 && r.done >= r.total) return;   // it actually finished — nothing to resume
        crewResumeId_ = r.runId;
        const QString task = r.task.trimmed().isEmpty() ? tr("a crew run") : r.task.trimmed();
        crewBannerText_->setText(
            tr("A crew run was interrupted here — “%1”  (%2/%3 subtasks done)")
                .arg(task.left(70)).arg(r.done).arg(r.total));
        crewBanner_->show();
        return;
    }
}

void MainWindow::onPaneClosed(const QString& id) {
    if (id == viewPaneId("board")) board_ = nullptr;
    else if (id == viewPaneId("editor")) editor_ = nullptr;
    else if (id == viewPaneId("files")) files_ = nullptr;
    // The pane owns its content widget, so a closed terminal's PTY dies with it.
    saveSession();
}

void MainWindow::onThemeChanged(const QString& name) {
    Theme::apply(qApp, name);
    canvas_->refreshTheme();
    if (board_) board_->refreshTheme();
    update();
}

void MainWindow::status(const QString& msg) { statusBar()->showMessage(msg, 6000); }

// ---- PaneHost --------------------------------------------------------------

QString MainWindow::currentModel() const {
    return models_ ? models_->currentText() : QString();
}

QString MainWindow::currentBackend() const {
    // The desktop drives Ollama; per-role backends are a crew-launch concern.
    return QStringLiteral("ollama");
}

void MainWindow::openFile(const QString& path) {
    ensureEditor();
    if (editor_) editor_->openFile(path);
    if (Pane* p = canvas_->pane(viewPaneId("editor"))) canvas_->raisePane(p);
}

void MainWindow::runInTerminal(const QString& command) {
    // Every pane that runs `ollamadev …` goes through here, so this one rewrite
    // makes the whole app drive the C++ CLI it ships instead of a PATH build.
    const QString cmd = withOdvCli(command);
    Pane* p = addTerminal(QStringLiteral("term_%1").arg(++termSeq_), project_, QString(), QRectF());
    if (!p) return;
    if (auto* t = p->content()->findChild<TerminalWidget*>())
        t->sendText(cmd + QLatin1Char('\n'));
    else if (auto* t2 = qobject_cast<TerminalWidget*>(p->content()))
        t2->sendText(cmd + QLatin1Char('\n'));
}

// ---- Ollama presence -------------------------------------------------------

void MainWindow::pingOllama() {
    const QString host =
        Config::str(QStringLiteral("ollama.host"), QStringLiteral("http://localhost:11434"));
    QNetworkRequest req{QUrl(host + "/api/tags")};
    OllamaBackend::applyAuth(req);
    req.setTransferTimeout(3000);
    QNetworkReply* reply = net_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const Theme::Colors c = Theme::currentColors();
        if (reply->error() != QNetworkReply::NoError) {
            conn_->setStyleSheet(QStringLiteral("color:%1;").arg(c.err.name()));
            conn_->setToolTip(tr("Ollama unreachable — %1").arg(reply->errorString()));
            if (!onboardShown_) {
                onboardShown_ = true;
                status(tr("Ollama isn't running. Start it (ollama serve), then Add → Start → "
                          "Setup to pick a model."));
            }
            return;
        }
        conn_->setStyleSheet(QStringLiteral("color:%1;").arg(c.ok.name()));
        conn_->setToolTip(tr("Ollama connected"));

        QStringList tags;
        const QJsonArray arr =
            QJsonDocument::fromJson(reply->readAll()).object().value("models").toArray();
        for (const QJsonValue& v : arr) tags << v.toObject().value("name").toString();
        tags.removeAll(QString());
        if (tags.isEmpty()) {
            // Connected but empty — the one real first-run dead end. Point at Setup
            // once, and open the Start pane so the button is right there.
            if (!onboardShown_) {
                onboardShown_ = true;
                status(tr("Ollama has no models yet — Add → Start → Setup recommends and pulls "
                          "one for your hardware."));
                if (PaneRegistry::instance().find(QStringLiteral("start")))
                    addPaneOfKind(QStringLiteral("start"));
            }
            return;
        }

        QStringList have;
        for (int i = 0; i < models_->count(); ++i) have << models_->itemText(i);
        if (have == tags) return;  // no churn while the user has the combo open

        const QString want = models_->currentText().isEmpty()
                                 ? Config::str(QStringLiteral("ollama.defaultModel"), QString())
                                 : models_->currentText();
        QSignalBlocker block(models_);
        models_->clear();
        models_->addItems(tags);
        const int i = models_->findText(want);
        models_->setCurrentIndex(i >= 0 ? i : 0);
    });
}

// ---- session ---------------------------------------------------------------

QString MainWindow::workspacesFile() {
    return QDir::homePath() + QStringLiteral("/.ollamadev/workspaces.json");
}

// 'ws_' + sha1(abs path) truncated to 10 hex chars — byte-for-byte the id the PHP
// Workspaces::add() generates, so both apps agree on which entry is which.
QString MainWindow::workspaceId(const QString& absPath) {
    const QByteArray h =
        QCryptographicHash::hash(absPath.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QStringLiteral("ws_") + QString::fromLatin1(h.left(10));
}

// ---- project switching ------------------------------------------------------

void MainWindow::buildProjectMenu(QMenu* menu) {
    menu->clear();
    connect(menu->addAction(tr("Open folder…\tCtrl+O")), &QAction::triggered, this,
            [this] { chooseProject(); });

    // The workspaces the CLI already knows about (`ollamadev ws add`). Both apps read
    // and write the same ~/.ollamadev/workspaces.json, so a folder bookmarked in one
    // shows up in the other.
    const QVector<Workspace> saved = Workspaces::all();
    if (saved.isEmpty()) return;

    menu->addSeparator();
    for (const Workspace& w : saved) {
        if (w.path == project_) continue;  // you are already here
        QAction* a = menu->addAction(QStringLiteral("%1   %2").arg(w.name, w.path));
        const QString path = w.path;
        connect(a, &QAction::triggered, this, [this, path] { openProject(path); });
    }
}

void MainWindow::chooseProject() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Open a project folder"), project_, QFileDialog::ShowDirsOnly);
    if (!dir.isEmpty()) openProject(dir);
}

void MainWindow::openProject(const QString& path) {
    const QString abs = QDir(path).absolutePath();
    if (abs == project_) return;
    if (!QFileInfo(abs).isDir()) {
        status(tr("no such folder: %1").arg(abs));
        return;
    }

    // Save the layout of the project we are LEAVING, before anything moves.
    saveSession();

    // Tear the canvas down. Terminals die with their panes — which is correct: a
    // shell sitting in the old project is not something you want silently carried
    // into the new one.
    restoring_ = true;
    for (Pane* p : canvas_->panes()) canvas_->removePane(p);

    project_ = abs;
    wsId_ = workspaceId(abs);

    // THE PART THAT ACTUALLY MATTERS. Nearly everything downstream — the crew, the
    // git pane, the file tree, every tool a coder runs — resolves against the
    // working directory or the tool root. Change one and forget the other and you
    // get an app that LOOKS like it opened your project while quietly still editing
    // the old one.
    QDir::setCurrent(abs);
    Tools::setThreadRoot(abs);
    Config::load();  // a project may carry its own ./.ollamadev.json

    // Bookmark it, so `ollamadev ws list` and this menu both know about it next time.
    Workspaces::add(abs);

    setWindowTitle(QStringLiteral("OllamaDev ADE — %1").arg(QFileInfo(abs).fileName()));
    projectBtn_->setText(QFileInfo(abs).fileName());
    projectBtn_->setToolTip(abs);

    restoring_ = false;
    loadSession();  // this project's own saved layout, or a fresh one
    offerCrewResumeIfAny();  // and any crew run interrupted in this project
    status(tr("opened %1").arg(abs));
}

QJsonObject MainWindow::captureState() const {
    QJsonObject state;

    QJsonArray terms;
    QJsonObject panes;
    QJsonArray extras;
    for (Pane* p : canvas_->panes()) {
        const QString id = p->id();
        // For the maximised pane, persist the geometry it un-maximises TO, not its
        // current viewport-filling rect — otherwise restore re-maximises from the
        // full-size rect and the original size/position is lost forever.
        const QRectF g =
            id == canvas_->maximisedId() ? canvas_->preMaximiseGeom() : p->geometryF();

        if (auto* term = qobject_cast<TerminalWidget*>(p->content())) {
            QJsonObject t = geomToJson(g);
            t.insert("id", id);
            t.insert("kind", term->property("odvKind").toString());
            t.insert("cwd", term->property("odvCwd").toString());
            t.insert("backend", QStringLiteral("ollama"));
            t.insert("model", models_ ? models_->currentText() : QString());
            t.insert("z", p->zValue());
            t.insert("replay", term->snapshot());
            terms.append(t);
            continue;
        }

        // The four built-in views keep the PHP-compatible `panes` map.
        if (id.startsWith(QLatin1String("__pop_"))) {
            const QString view = id.mid(6, id.size() - 8);  // __pop_<view>__
            if (isBuiltinView(view)) {
                panes.insert(view, geomToJson(g));
                continue;
            }
        }

        // Everything else is a registry pane. Reopen it next session at its geometry
        // and z, with its inner content when the spec knows how to snapshot it.
        const QString kind = paneKindFromId(id);
        if (const PaneSpec* spec = PaneRegistry::instance().find(kind)) {
            QJsonObject o = geomToJson(g);
            o.insert("kind", kind);
            o.insert("id", id);
            o.insert("z", p->zValue());
            if (spec->snapshot) o.insert("content", spec->snapshot(p->content()));
            extras.append(o);
        }
    }

    state.insert("terminals", terms);
    state.insert("panes", panes);
    state.insert("extraPanes", extras);
    state.insert("editorTabs", editor_ ? editor_->snapshot() : QJsonArray());
    // The PHP app reads pan.x / pan.y and ignores anything else in the object, so
    // carrying the zoom here keeps one schema for both.
    const QPointF o = canvas_->viewOrigin();
    state.insert("pan", QJsonObject{{"x", o.x()}, {"y", o.y()}, {"zoom", canvas_->zoom()}});
    state.insert("zoomed", canvas_->maximisedId().isEmpty() ? QJsonValue()
                                                            : QJsonValue(canvas_->maximisedId()));
    return state;
}

void MainWindow::restoreState(const QJsonObject& state) {
    restoring_ = true;

    const QJsonObject panes = state.value("panes").toObject();
    const QJsonArray extras = state.value("extraPanes").toArray();
    if (panes.isEmpty() && state.value("terminals").toArray().isEmpty() && extras.isEmpty()) {
        // First run in this project: a small, tidy starter cluster in the top-left,
        // NOT a workbench that eats the whole screen. It's an infinite canvas — the
        // point is room to spread out, so defaults open compact and leave it to the
        // user to grow (or maximise) the pane they're actually working in.
        ensureFiles(QRectF(16, 16, 250, 300));
        ensureEditor(QRectF(282, 16, 430, 300));
        addTerminal(QStringLiteral("term_%1").arg(++termSeq_), project_, QString(),
                    QRectF(16, 332, 696, 200));
        restoring_ = false;
        return;
    }

    for (const QString& view : panes.keys()) {
        const QRectF g = geomFromJson(panes.value(view).toObject());
        if (view == "board") ensureBoard(g);
        else if (view == "editor") ensureEditor(g);
        else if (view == "files") ensureFiles(g);
        else if (view == "settings") ensureSettings(g);
        // Unknown views (graph, browser, … from the PHP app) are ignored here but
        // NOT dropped from the file — saveSession only rewrites `state`.
    }

    for (const QJsonValue& v : state.value("editorTabs").toArray()) {
        const QJsonObject t = v.toObject();
        ensureEditor();
        if (t.value("dirty").toBool())
            editor_->openWith(t.value("path").toString(), t.value("name").toString(),
                              t.value("content").toString(), true);
        else editor_->openFile(t.value("path").toString());
    }

    for (const QJsonValue& v : state.value("terminals").toArray()) {
        const QJsonObject t = v.toObject();
        const QString kind = t.value("kind").toString();
        const bool isCli = !kind.isEmpty() && kind != QLatin1String("shell");

        // A CLI pane relaunches its REPL below, and the REPL auto-resumes this
        // folder's session — so replaying its old scrollback would only paint stale,
        // wrong-width output (box frames, an old prompt) that then overlaps the fresh
        // CLI and looks garbled. Don't replay it for CLIs; a plain shell keeps its
        // scrollback.
        Pane* p = addTerminal(t.value("id").toString(QStringLiteral("term_%1").arg(++termSeq_)),
                              t.value("cwd").toString(project_),
                              isCli ? QString() : t.value("replay").toString(), geomFromJson(t));
        if (p && t.contains("z")) p->setZValue(t.value("z").toDouble());

        // A CLI pane (a terminal that was running ollamadev/claude/…) comes back as
        // that CLI, not a bare shell: relaunch its REPL just like addCliTerminal.
        if (p && isCli) {
            if (auto* term = qobject_cast<TerminalWidget*>(p->content())) {
                term->setProperty("odvKind", kind);
                p->setTitle(kind);
                term->sendText(QStringLiteral("clear; ") + withOdvCli(kind) + QLatin1Char('\n'));
            }
        }
    }

    // Every other canvas pane (chat, git, graph, tasks, …). Reopened via the
    // registry, before pan/zoom so a maximised one can still be re-focused below.
    for (const QJsonValue& v : extras) {
        const QJsonObject o = v.toObject();
        const QString kind = o.value("kind").toString();
        const PaneSpec* spec = PaneRegistry::instance().find(kind);
        if (!spec) continue;  // a pane kind this build no longer ships
        const QString id = o.value("id").toString(viewPaneId(kind));
        Pane* p = addRegistryPane(*spec, id, geomFromJson(o));
        if (!p) continue;
        if (o.contains("z")) p->setZValue(o.value("z").toDouble());
        if (spec->restore && o.contains("content"))
            spec->restore(p->content(), o.value("content").toObject());
        // Stacked panes share termSeq_ with terminals; keep it ahead of restored ids.
        if (!spec->singleton) {
            const int seq = id.mid(kind.size() + 1).toInt();
            if (seq > termSeq_) termSeq_ = seq;
        }
    }

    const QJsonObject pan = state.value("pan").toObject();
    canvas_->setView(QPointF(pan.value("x").toDouble(), pan.value("y").toDouble()),
                     pan.value("zoom").toDouble(1.0));

    if (Pane* z = canvas_->pane(state.value("zoomed").toString())) canvas_->toggleFocus(z);
    restoring_ = false;
}

void MainWindow::loadSession() {
    QFile f(workspacesFile());
    QJsonObject doc;
    if (f.open(QIODevice::ReadOnly)) doc = QJsonDocument::fromJson(f.readAll()).object();

    QJsonObject state;
    for (const QJsonValue& v : doc.value("workspaces").toArray()) {
        const QJsonObject w = v.toObject();
        if (w.value("path").toString() == project_) {
            state = w.value("state").toObject();
            break;
        }
    }
    restoreState(state);
    status(state.isEmpty() ? tr("new workspace") : tr("workspace restored"));
}

void MainWindow::autosave() {
    // A transiently empty window (mid-restore, or the last pane just closed while
    // another is being built) must NEVER overwrite a real project's layout.
    if (restoring_ || canvas_->paneCount() == 0) return;
    saveSession();
}

void MainWindow::saveSession() {
    if (canvas_->paneCount() == 0) return;

    const QString path = workspacesFile();
    QDir().mkpath(QFileInfo(path).absolutePath());

    // Read the existing file so we only rewrite THIS project's slice and leave every
    // other workspace (and any CLI/PHP-written fields) untouched.
    QJsonObject doc;
    QFile in(path);
    if (in.open(QIODevice::ReadOnly)) {
        const QByteArray bytes = in.readAll();
        in.close();
        QJsonParseError err{};
        const QJsonDocument parsed = QJsonDocument::fromJson(bytes, &err);
        if (err.error == QJsonParseError::NoError) {
            doc = parsed.object();
        } else if (!bytes.trimmed().isEmpty()) {
            // The file has bytes but won't parse — a truncated write from an older
            // build that saved non-atomically, or a half-written file. Overwriting it
            // blind would erase every OTHER workspace it still holds, so preserve a
            // copy once for recovery instead of silently clobbering it.
            const QString bak = path + QStringLiteral(".corrupt");
            if (!QFileInfo::exists(bak)) QFile::copy(path, bak);
        }
    }

    QJsonArray list = doc.value("workspaces").toArray();
    const QJsonObject state = captureState();
    bool found = false;
    for (int i = 0; i < list.size(); ++i) {
        QJsonObject w = list[i].toObject();
        if (w.value("path").toString() != project_) continue;
        // Rewrite ONLY the fields this app owns; anything the CLI or the PHP app
        // put here (name, notes, …) survives untouched.
        w.insert("state", state);
        w.insert("lastOpened", QDateTime::currentDateTime().toString(Qt::ISODate));
        list[i] = w;
        found = true;
        break;
    }
    if (!found) {
        list.append(QJsonObject{{"id", wsId_},
                                {"name", QFileInfo(project_).fileName()},
                                {"path", project_},
                                {"lastOpened", QDateTime::currentDateTime().toString(Qt::ISODate)},
                                {"state", state}});
    }
    doc.insert("workspaces", list);
    doc.insert("active", wsId_);

    // Atomic write: QSaveFile writes a temp file and renames it into place, so a
    // crash or a kill mid-write leaves the PREVIOUS good file intact instead of a
    // truncated one that fails to parse and wipes the whole restore. autosave calls
    // this every few seconds, so an interrupted write here was the likeliest way to
    // lose a layout — the packaged app just hits it more when a GPU/driver crash
    // restarts it. Session::save already writes this way; the canvas needs it too.
    QSaveFile out(path);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QJsonDocument(doc).toJson(QJsonDocument::Indented));
        out.commit();
    }
}

// ---- quit ------------------------------------------------------------------

void MainWindow::closeEvent(QCloseEvent* e) {
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Quit"));
    box.setText(tr("Quit OllamaDev?"));
    const bool unsaved = editor_ && editor_->hasUnsaved();
    box.setInformativeText(unsaved ? tr("An editor tab has unsaved changes. They are kept in the "
                                        "workspace and restored next time.")
                                   : tr("Your canvas layout and terminals are saved and restored "
                                        "next time."));
    QPushButton* quit = box.addButton(tr("Quit"), QMessageBox::AcceptRole);
    QPushButton* cancel = box.addButton(tr("Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(cancel);
    box.exec();

    if (box.clickedButton() != quit) {
        e->ignore();
        return;
    }
    autosave_->stop();
    saveSession();  // state first, THEN exit
    e->accept();
}

}  // namespace odv
