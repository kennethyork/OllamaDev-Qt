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
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
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
#include "EditorPane.h"
#include "FilesPane.h"
#include "ManageDialogs.h"
#include "Pane.h"
#include "TerminalWidget.h"
#include "Theme.h"
#include "ThemeDialog.h"
#include "Tools.h"

namespace odv {
namespace {

// Pane ids for the singleton views. Deliberately the SAME strings the PHP app
// used (`'__pop_' + view + '__'`) so a workspaces.json written by either app
// round-trips through the other.
QString viewPaneId(const QString& view) { return QStringLiteral("__pop_%1__").arg(view); }

QRectF geomFromJson(const QJsonObject& o) {
    if (!o.contains("w")) return QRectF();
    return QRectF(o.value("x").toDouble(), o.value("y").toDouble(), o.value("w").toDouble(),
                  o.value("h").toDouble());
}

QJsonObject geomToJson(const QRectF& r) {
    return QJsonObject{{"x", r.x()}, {"y", r.y()}, {"w", r.width()}, {"h", r.height()}};
}

QString userShell() { return qEnvironmentVariable("SHELL", QStringLiteral("/bin/bash")); }

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    project_ = QDir::currentPath();
    wsId_ = workspaceId(project_);

    setWindowTitle(QStringLiteral("OllamaDev ADE — %1").arg(QFileInfo(project_).fileName()));
    resize(1440, 900);

    registerExtraPanes();  // populate the registry before the Add menu is built

    canvas_ = new Canvas(this);
    setCentralWidget(canvas_);
    connect(canvas_, &Canvas::paneClosed, this, &MainWindow::onPaneClosed);
    connect(canvas_, &Canvas::contextMenuRequestedAt, this, &MainWindow::onCanvasContextMenu);

    buildTopBar();
    statusBar()->showMessage(tr("ready"));

    net_ = new QNetworkAccessManager(this);
    ping_ = new QTimer(this);
    ping_->setInterval(5000);
    connect(ping_, &QTimer::timeout, this, &MainWindow::pingOllama);
    ping_->start();
    pingOllama();

    loadSession();

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
    perms_->addItem(tr("✋ Ask · confirm each change"), QStringLiteral("ask"));
    perms_->addItem(tr("⚡ Auto · run tools without asking"), QStringLiteral("auto"));
    perms_->addItem(tr("🔒 Read-only · no changes"), QStringLiteral("readonly"));
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
    mmenu->addSeparator();
    connect(mmenu->addAction(tr("Crew roles…")), &QAction::triggered, this,
            [this] { ManageDialogs::openRoles(*this); });
    connect(mmenu->addAction(tr("Skills…")), &QAction::triggered, this,
            [this] { ManageDialogs::openSkills(*this); });
    connect(mmenu->addAction(tr("Hooks…")), &QAction::triggered, this,
            [this] { ManageDialogs::openHooks(*this); });
    mmenu->addSeparator();
    connect(mmenu->addAction(tr("Theme editor…")), &QAction::triggered, this,
            [this] { ThemeDialog::open(this); });
    manage->setMenu(mmenu);
    row->addWidget(manage);

    row->addStretch(1);

    auto* cwd = new QLabel(project_, bar);
    cwd->setStyleSheet(
        QStringLiteral("color:%1;").arg(Theme::currentColors().faint.name()));
    row->addWidget(cwd);

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
    QAction* center = menu->addAction(tr("Center canvas"));
    connect(center, &QAction::triggered, canvas_, &Canvas::centerAll);
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
        if (spec->singleton) {
            if (Pane* p = canvas_->pane(id)) {
                canvas_->raisePane(p);
                return;
            }
        }
        if (QWidget* w = spec->factory(*this)) canvas_->addPane(id, spec->title, w, QRectF());
    }
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
    // The command is just the id — cursor-agent's binary is literally "cursor-agent".
    term->sendText(cliId + QLatin1Char('\n'));
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
    form->addRow(tr("Ollama host"), host);

    auto* temp = new QDoubleSpinBox(w);
    temp->setRange(0.0, 2.0);
    temp->setSingleStep(0.1);
    temp->setValue(Config::number(QStringLiteral("agents.coder.temperature"), 0.7));
    form->addRow(tr("Temperature"), temp);

    auto* save = new QPushButton(tr("Save"), w);
    save->setProperty("cta", true);
    v->addWidget(save, 0, Qt::AlignLeft);
    v->addStretch(1);

    connect(save, &QPushButton::clicked, this, [this, host, temp] {
        Config::setPref(QStringLiteral("ollama.host"), host->text().trimmed());
        Config::setPref(QStringLiteral("agents.coder.temperature"), temp->value());
        status(tr("settings saved to ade-prefs.json"));
        pingOllama();
    });
    return w;
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
    Pane* p = addTerminal(QStringLiteral("term_%1").arg(++termSeq_), project_, QString(), QRectF());
    if (!p) return;
    if (auto* t = p->content()->findChild<TerminalWidget*>())
        t->sendText(command + QLatin1Char('\n'));
    else if (auto* t2 = qobject_cast<TerminalWidget*>(p->content()))
        t2->sendText(command + QLatin1Char('\n'));
}

// ---- Ollama presence -------------------------------------------------------

void MainWindow::pingOllama() {
    const QString host =
        Config::str(QStringLiteral("ollama.host"), QStringLiteral("http://localhost:11434"));
    QNetworkRequest req{QUrl(host + "/api/tags")};
    req.setTransferTimeout(3000);
    QNetworkReply* reply = net_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const Theme::Colors c = Theme::currentColors();
        if (reply->error() != QNetworkReply::NoError) {
            conn_->setStyleSheet(QStringLiteral("color:%1;").arg(c.err.name()));
            conn_->setToolTip(tr("Ollama unreachable — %1").arg(reply->errorString()));
            return;
        }
        conn_->setStyleSheet(QStringLiteral("color:%1;").arg(c.ok.name()));
        conn_->setToolTip(tr("Ollama connected"));

        QStringList tags;
        const QJsonArray arr =
            QJsonDocument::fromJson(reply->readAll()).object().value("models").toArray();
        for (const QJsonValue& v : arr) tags << v.toObject().value("name").toString();
        tags.removeAll(QString());
        if (tags.isEmpty()) return;

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

QJsonObject MainWindow::captureState() const {
    QJsonObject state;

    QJsonArray terms;
    QJsonObject panes;
    for (Pane* p : canvas_->panes()) {
        const QString id = p->id();
        const QRectF g = p->geometryF();
        if (id.startsWith(QLatin1String("__pop_"))) {
            const QString view = id.mid(6, id.size() - 8);  // __pop_<view>__
            panes.insert(view, geomToJson(g));
            continue;
        }
        auto* term = qobject_cast<TerminalWidget*>(p->content());
        if (!term) continue;
        QJsonObject t = geomToJson(g);
        t.insert("id", id);
        t.insert("kind", term->property("odvKind").toString());
        t.insert("cwd", term->property("odvCwd").toString());
        t.insert("backend", QStringLiteral("ollama"));
        t.insert("model", models_ ? models_->currentText() : QString());
        t.insert("z", p->zValue());
        t.insert("replay", term->snapshot());
        terms.append(t);
    }

    state.insert("terminals", terms);
    state.insert("panes", panes);
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
    if (panes.isEmpty() && state.value("terminals").toArray().isEmpty()) {
        // First run in this project: open a real workbench, not an empty board.
        ensureFiles(QRectF(16, 16, 320, 520));
        ensureEditor(QRectF(356, 16, 700, 520));
        addTerminal(QStringLiteral("term_%1").arg(++termSeq_), project_, QString(),
                    QRectF(16, 556, 1040, 320));
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
        Pane* p = addTerminal(t.value("id").toString(QStringLiteral("term_%1").arg(++termSeq_)),
                              t.value("cwd").toString(project_), t.value("replay").toString(),
                              geomFromJson(t));
        if (p && t.contains("z")) p->setZValue(t.value("z").toDouble());
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

    QJsonObject doc;
    QFile in(path);
    if (in.open(QIODevice::ReadOnly)) {
        doc = QJsonDocument::fromJson(in.readAll()).object();
        in.close();
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

    QFile out(path);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        out.write(QJsonDocument(doc).toJson(QJsonDocument::Indented));
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
