// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "EditorPane.h"

#include <QAbstractButton>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonObject>
#include <QMessageBox>
#include <QKeyEvent>
#include <QPainter>
#include <QPushButton>
#include <QShortcut>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include "InlineCompleter.h"
#include "Theme.h"

namespace odv {

// ---- gutter ---------------------------------------------------------------

LineGutter::LineGutter(CodeEdit* editor) : QWidget(editor), editor_(editor) {}

QSize LineGutter::sizeHint() const { return QSize(editor_->gutterWidth(), 0); }

void LineGutter::paintEvent(QPaintEvent* e) { editor_->paintGutter(e); }

// ---- editor ---------------------------------------------------------------

CodeEdit::CodeEdit(QWidget* parent) : QPlainTextEdit(parent) {
    gutter_ = new LineGutter(this);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);

    connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeEdit::updateGutterWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeEdit::updateGutter);
    updateGutterWidth();

    // Inline FIM completion. A short debounce means we ask only after typing pauses,
    // not on every keystroke; a fresh edit or a cursor move drops any pending ghost.
    completer_ = new InlineCompleter(this);
    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(350);
    connect(debounce_, &QTimer::timeout, this, &CodeEdit::requestCompletion);
    connect(completer_, &InlineCompleter::suggestion, this, &CodeEdit::showGhost);
    connect(this, &QPlainTextEdit::textChanged, this, [this] {
        clearGhost();  // the buffer changed — any ghost is stale
        if (InlineCompleter::enabled()) debounce_->start();
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        if (ghostAt_ >= 0 && textCursor().position() != ghostAt_) clearGhost();
    });
}

void CodeEdit::requestCompletion() {
    if (!InlineCompleter::enabled() || !hasFocus()) return;
    QTextCursor cur = textCursor();
    if (cur.hasSelection()) return;  // a completion mid-selection makes no sense
    const int pos = cur.position();
    const QString all = toPlainText();
    // Bound the context so a huge file doesn't make each keystroke-pause slow: the
    // model only needs the local neighbourhood to fill the hole.
    const int prefixFrom = qMax(0, pos - 2000);
    const int suffixTo = qMin(all.size(), pos + 1000);
    completer_->request(all.mid(prefixFrom, pos - prefixFrom), all.mid(pos, suffixTo - pos));
}

void CodeEdit::showGhost(const QString& s) {
    // Ignore an empty/whitespace-only suggestion, or one that raced in after the
    // cursor moved (the position it was computed for no longer holds).
    const QString trimmed = s.left(400);  // never draw a wall of text
    if (trimmed.trimmed().isEmpty() || !hasFocus() || textCursor().hasSelection()) {
        clearGhost();
        return;
    }
    ghost_ = trimmed;
    ghostAt_ = textCursor().position();
    viewport()->update();
}

void CodeEdit::clearGhost() {
    if (ghostAt_ < 0 && ghost_.isEmpty()) return;
    ghost_.clear();
    ghostAt_ = -1;
    viewport()->update();
}

void CodeEdit::acceptGhost() {
    if (ghost_.isEmpty()) return;
    const QString text = ghost_;
    clearGhost();
    QTextCursor cur = textCursor();
    cur.insertText(text);  // one undo step, and it lands as a normal edit (marks dirty)
    setTextCursor(cur);
}

int CodeEdit::gutterWidth() const {
    int digits = 1;
    for (int n = qMax(1, blockCount()); n >= 10; n /= 10) ++digits;
    return 12 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeEdit::updateGutterWidth() { setViewportMargins(gutterWidth(), 0, 0, 0); }

void CodeEdit::updateGutter(const QRect& r, int dy) {
    if (dy) gutter_->scroll(0, dy);
    else gutter_->update(0, r.y(), gutter_->width(), r.height());
    if (r.contains(viewport()->rect())) updateGutterWidth();
}

void CodeEdit::resizeEvent(QResizeEvent* e) {
    QPlainTextEdit::resizeEvent(e);
    const QRect cr = contentsRect();
    gutter_->setGeometry(QRect(cr.left(), cr.top(), gutterWidth(), cr.height()));
}

void CodeEdit::paintEvent(QPaintEvent* e) {
    QPlainTextEdit::paintEvent(e);
    if (ghost_.isEmpty() || ghostAt_ < 0 || textCursor().position() != ghostAt_) return;

    QPainter p(viewport());
    p.setFont(font());
    p.setPen(Theme::currentColors().faint);  // a muted hint, clearly not real text
    const QRect cr = cursorRect();
    const int lineH = fontMetrics().height();
    const int contLeft = qRound(contentOffset().x() + document()->documentMargin());
    const QStringList lines = ghost_.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const int x = i == 0 ? cr.left() : contLeft;
        const int y = cr.top() + i * lineH + fontMetrics().ascent();
        p.drawText(x, y, lines.at(i));
    }
}

void CodeEdit::keyPressEvent(QKeyEvent* e) {
    if (ghostAt_ >= 0 && !ghost_.isEmpty()) {
        if (e->key() == Qt::Key_Tab) {  // Tab accepts the whole suggestion
            acceptGhost();
            e->accept();
            return;
        }
        if (e->key() == Qt::Key_Escape) {  // Esc dismisses it
            clearGhost();
            e->accept();
            return;
        }
        // Any other key edits the buffer — textChanged will drop the stale ghost.
    }
    QPlainTextEdit::keyPressEvent(e);
}

void CodeEdit::focusOutEvent(QFocusEvent* e) {
    clearGhost();
    QPlainTextEdit::focusOutEvent(e);
}

void CodeEdit::paintGutter(QPaintEvent* e) {
    const Theme::Colors c = Theme::currentColors();
    QPainter p(gutter_);
    p.fillRect(e->rect(), c.bg2);

    QTextBlock block = firstVisibleBlock();
    int num = block.blockNumber();
    qreal top = blockBoundingGeometry(block).translated(contentOffset()).top();
    qreal bottom = top + blockBoundingRect(block).height();
    const int cur = textCursor().blockNumber();

    while (block.isValid() && top <= e->rect().bottom()) {
        if (block.isVisible() && bottom >= e->rect().top()) {
            p.setPen(num == cur ? c.accent : c.faint);
            p.drawText(0, int(top), gutter_->width() - 6, fontMetrics().height(),
                       Qt::AlignRight | Qt::AlignVCenter, QString::number(num + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + blockBoundingRect(block).height();
        ++num;
    }
}

// ---- pane -----------------------------------------------------------------

EditorPane::EditorPane(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    tabs_ = new QTabWidget(this);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setDocumentMode(true);
    v->addWidget(tabs_);

    connect(tabs_, &QTabWidget::tabCloseRequested, this, &EditorPane::onTabCloseRequested);

    // WidgetWithChildren, not the window: the pane lives on a canvas next to
    // terminals, and Ctrl+S must not steal from whatever else has focus.
    auto* save = new QShortcut(QKeySequence::Save, this);
    save->setContext(Qt::WidgetWithChildrenShortcut);
    connect(save, &QShortcut::activated, this, &EditorPane::saveCurrent);
}

CodeEdit* EditorPane::editorAt(int i) const {
    return qobject_cast<CodeEdit*>(tabs_->widget(i));
}

int EditorPane::tabCount() const { return tabs_->count(); }

int EditorPane::indexOfPath(const QString& path) const {
    for (int i = 0; i < tabs_->count(); ++i)
        if (CodeEdit* e = editorAt(i); e && e->path == path) return i;
    return -1;
}

void EditorPane::refreshTabLabel(CodeEdit* ed) {
    const int i = tabs_->indexOf(ed);
    if (i < 0) return;
    tabs_->setTabText(i, (ed->dirty ? QStringLiteral("• ") : QString()) + ed->name);
    tabs_->setTabToolTip(i, ed->path);
}

void EditorPane::markDirty(CodeEdit* ed, bool dirty) {
    if (ed->dirty == dirty) return;
    ed->dirty = dirty;
    refreshTabLabel(ed);
}

void EditorPane::openFile(const QString& path) {
    const int existing = indexOfPath(path);
    if (existing >= 0) {
        tabs_->setCurrentIndex(existing);
        return;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        emit statusMessage(tr("could not open %1").arg(path));
        return;
    }
    QTextStream in(&f);
    openWith(path, QFileInfo(path).fileName(), in.readAll(), false);
}

void EditorPane::openWith(const QString& path, const QString& name, const QString& content,
                          bool dirty) {
    const int existing = indexOfPath(path);
    if (existing >= 0) {
        tabs_->setCurrentIndex(existing);
        return;
    }
    auto* ed = new CodeEdit(tabs_);
    ed->path = path;
    ed->name = name.isEmpty() ? QFileInfo(path).fileName() : name;
    ed->setPlainText(content);
    ed->dirty = dirty;
    // Connect AFTER setPlainText, or the initial fill would mark the tab dirty.
    connect(ed, &QPlainTextEdit::textChanged, this, [this, ed] { markDirty(ed, true); });

    const int i = tabs_->addTab(ed, ed->name);
    tabs_->setCurrentIndex(i);
    refreshTabLabel(ed);
}

void EditorPane::saveCurrent() {
    CodeEdit* ed = editorAt(tabs_->currentIndex());
    if (!ed) return;
    QFile f(ed->path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        emit statusMessage(tr("could not write %1").arg(ed->path));
        return;
    }
    QTextStream out(&f);
    out << ed->toPlainText();
    f.close();
    markDirty(ed, false);
    emit statusMessage(tr("saved %1").arg(ed->name));
}

void EditorPane::onTabCloseRequested(int i) {
    CodeEdit* ed = editorAt(i);
    if (!ed) return;
    if (ed->dirty) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(tr("Unsaved changes"));
        box.setText(tr("%1 has unsaved changes. Close it anyway?").arg(ed->name));
        QPushButton* save = box.addButton(tr("Save && close"), QMessageBox::AcceptRole);
        QPushButton* discard = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
        box.addButton(tr("Cancel"), QMessageBox::RejectRole);
        box.setDefaultButton(save);
        box.exec();
        if (box.clickedButton() == save) {
            tabs_->setCurrentIndex(i);
            saveCurrent();
            if (ed->dirty) return;  // the write failed — keep the buffer
        } else if (box.clickedButton() != discard) {
            return;
        }
    }
    tabs_->removeTab(i);
    ed->deleteLater();
}

bool EditorPane::hasUnsaved() const {
    for (int i = 0; i < tabs_->count(); ++i)
        if (CodeEdit* e = editorAt(i); e && e->dirty) return true;
    return false;
}

// Same shape as Editor.snapshot() in app.js: clean tabs are reloaded from disk on
// restore, so only a dirty tab pays to carry its buffer.
QJsonArray EditorPane::snapshot() const {
    QJsonArray out;
    for (int i = 0; i < tabs_->count(); ++i) {
        CodeEdit* e = editorAt(i);
        if (!e) continue;
        QJsonObject o{{"path", e->path}, {"name", e->name}};
        if (e->dirty) {
            o.insert("dirty", true);
            o.insert("content", e->toPlainText());
        }
        out.append(o);
    }
    return out;
}

}  // namespace odv
