#pragma once
#include <QJsonObject>
#include <QMainWindow>
#include <QString>

#include "PaneHost.h"

class QComboBox;
class QLabel;
class QNetworkAccessManager;
class QTimer;

namespace odv {

class BoardPane;
class Canvas;
class EditorPane;
class FilesPane;
class Pane;

class MainWindow : public QMainWindow, public PaneHost {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // PaneHost — the surface registered panes reach the app through.
    QString project() const override { return project_; }
    QString currentModel() const override;
    QString currentBackend() const override;
    void setStatus(const QString& msg) override { status(msg); }
    void openFile(const QString& path) override;
    void runInTerminal(const QString& command) override;
    QWidget* window() override { return this; }

protected:
    // Never exits on the first click: asks, then saves, then closes. The PHP app
    // did this with a WindowClosing intercept that cancelled the native close and
    // called back into the page; here it is simply closeEvent().
    void closeEvent(QCloseEvent* e) override;

private slots:
    void addPaneOfKind(const QString& kind);
    void pingOllama();
    void autosave();
    void onPaneClosed(const QString& id);
    void onCanvasContextMenu(const QPoint& globalPos, const QPointF& worldPos);
    void onThemeChanged(const QString& name);

private:
    void buildTopBar();
    void buildAddMenu(class QMenu* menu);

    Pane* addTerminal(const QString& id, const QString& cwd, const QString& replay,
                      const QRectF& geom);
    Pane* addCliTerminal(const QString& cliId);  // a terminal that opens into a CLI's REPL
    Pane* ensureBoard(const QRectF& geom = QRectF());
    Pane* ensureEditor(const QRectF& geom = QRectF());
    Pane* ensureFiles(const QRectF& geom = QRectF());
    Pane* ensureSettings(const QRectF& geom = QRectF());
    QWidget* buildSettingsWidget();

    void status(const QString& msg);

    // ---- session (~/.ollamadev/workspaces.json, same schema as the PHP app) ----
    static QString workspacesFile();
    static QString workspaceId(const QString& absPath);
    QJsonObject captureState() const;
    void restoreState(const QJsonObject& state);
    void loadSession();
    void saveSession();

    Canvas* canvas_ = nullptr;
    BoardPane* board_ = nullptr;
    EditorPane* editor_ = nullptr;
    FilesPane* files_ = nullptr;

    QLabel* conn_ = nullptr;
    QComboBox* models_ = nullptr;
    QComboBox* perms_ = nullptr;
    QComboBox* themes_ = nullptr;

    QNetworkAccessManager* net_ = nullptr;
    QTimer* ping_ = nullptr;
    QTimer* autosave_ = nullptr;

    QString project_;   // the project root — the app's cwd, like the CLI
    QString wsId_;
    int termSeq_ = 0;
    bool restoring_ = false;
};

}  // namespace odv
