// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <QJsonObject>
#include <QMainWindow>
#include <QString>

#include "PaneHost.h"

class QComboBox;
class QLabel;
class QNetworkAccessManager;
class QTimer;

class QToolButton;

namespace odv {

class BoardPane;
class Canvas;
class EditorPane;
class FilesPane;
class Pane;

class MainWindow : public QMainWindow, public PaneHost {
    Q_OBJECT

public:
    // `startupPath`, when non-empty, is the project folder to open (an explicit
    // command-line argument). Empty means "reopen the last active workspace".
    explicit MainWindow(const QString& startupPath = {}, QWidget* parent = nullptr);
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
    void buildProjectMenu(class QMenu* menu);
    void chooseProject();               // Ctrl+O — pick a folder
    void openProject(const QString& path);  // switch project: re-root EVERYTHING
    void buildAddMenu(class QMenu* menu);
    void addVpsMcp();

    Pane* addTerminal(const QString& id, const QString& cwd, const QString& replay,
                      const QRectF& geom);
    Pane* addCliTerminal(const QString& cliId);  // a terminal that opens into a CLI's REPL
    Pane* ensureBoard(const QRectF& geom = QRectF());
    Pane* ensureEditor(const QRectF& geom = QRectF());
    Pane* ensureFiles(const QRectF& geom = QRectF());
    Pane* ensureSettings(const QRectF& geom = QRectF());
    // Build a registered pane at an explicit id + geometry (or re-raise it if the
    // id is already on the canvas). Shared by the Add menu and session restore.
    Pane* addRegistryPane(const struct PaneSpec& spec, const QString& id, const QRectF& geom);
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
    QToolButton* projectBtn_ = nullptr;

    QNetworkAccessManager* net_ = nullptr;
    QTimer* ping_ = nullptr;
    QTimer* autosave_ = nullptr;

    QString project_;   // the project root — the app's cwd, like the CLI
    QString wsId_;
    int termSeq_ = 0;
    bool restoring_ = false;
    bool onboardShown_ = false;  // first-run onboarding hint shown once
};

}  // namespace odv
