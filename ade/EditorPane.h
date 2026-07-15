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
#include <QJsonArray>
#include <QPlainTextEdit>
#include <QString>
#include <QWidget>

class QTabWidget;
class QTimer;

namespace odv {

class InlineCompleter;

// Gutter for CodeEdit. A plain QWidget whose paint is delegated back to the
// editor, which is the only thing that knows the block geometry.
class LineGutter : public QWidget {
    Q_OBJECT
public:
    explicit LineGutter(class CodeEdit* editor);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    class CodeEdit* editor_;
};

// One editor tab: a plain-text editor with a line-number gutter. No syntax
// highlighting — the PHP editor had none either, and adding one here would be
// the first thing in this app to need a grammar table.
class CodeEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CodeEdit(QWidget* parent = nullptr);

    int gutterWidth() const;
    void paintGutter(QPaintEvent* e);

    QString path;
    QString name;
    bool dirty = false;

protected:
    void resizeEvent(QResizeEvent* e) override;
    // Inline FIM completion: draw the ghost after the real text, accept it on Tab.
    void paintEvent(QPaintEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;

private slots:
    void updateGutterWidth();
    void updateGutter(const QRect& r, int dy);

private:
    void requestCompletion();          // debounced ask for a suggestion at the cursor
    void showGhost(const QString& s);  // a suggestion arrived
    void clearGhost();                 // it went stale (edit, move, blur, accept)
    void acceptGhost();                // Tab: splice the suggestion into the buffer

    LineGutter* gutter_;

    InlineCompleter* completer_ = nullptr;
    QTimer* debounce_ = nullptr;
    QString ghost_;       // the pending suggestion, drawn but not yet in the document
    int ghostAt_ = -1;    // cursor position the ghost belongs to; -1 == no ghost
};

// Tabbed plain-text editor. Ctrl+S saves; a dirty tab carries a • in its label
// and confirms before closing; the snapshot/restore shape matches Editor.snapshot()
// in app.js so workspaces.json stays readable by both apps.
class EditorPane : public QWidget {
    Q_OBJECT

public:
    explicit EditorPane(QWidget* parent = nullptr);

    void openFile(const QString& path);
    // Restore a tab verbatim (a dirty tab carries its unsaved buffer).
    void openWith(const QString& path, const QString& name, const QString& content, bool dirty);

    QJsonArray snapshot() const;  // [{path, name, dirty?, content?}]
    bool hasUnsaved() const;
    int tabCount() const;

signals:
    void statusMessage(const QString& msg);

private slots:
    void saveCurrent();
    void onTabCloseRequested(int i);

private:
    CodeEdit* editorAt(int i) const;
    int indexOfPath(const QString& path) const;
    void markDirty(CodeEdit* ed, bool dirty);
    void refreshTabLabel(CodeEdit* ed);

    QTabWidget* tabs_ = nullptr;
};

}  // namespace odv
