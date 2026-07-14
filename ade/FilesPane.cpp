// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "FilesPane.h"

#include <QAbstractButton>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeView>
#include <QVBoxLayout>

namespace odv {

FilesPane::FilesPane(const QString& root, QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    model_ = new QFileSystemModel(this);
    model_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    model_->setReadOnly(false);

    tree_ = new QTreeView(this);
    tree_->setModel(model_);
    tree_->setHeaderHidden(true);
    tree_->setAnimated(false);
    tree_->setIndentation(14);
    tree_->setSortingEnabled(false);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);  // rename goes through the menu
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    // Only the name column: size/type/date are noise in a 320px-wide pane.
    for (int c = 1; c < model_->columnCount(); ++c) tree_->hideColumn(c);
    v->addWidget(tree_);

    connect(tree_, &QTreeView::doubleClicked, this, &FilesPane::onActivated);
    connect(tree_, &QWidget::customContextMenuRequested, this, &FilesPane::onContextMenu);

    setRoot(root);
}

void FilesPane::setRoot(const QString& root) {
    root_ = root;
    const QModelIndex idx = model_->setRootPath(root);
    tree_->setRootIndex(idx);
}

void FilesPane::onActivated(const QModelIndex& idx) {
    if (!idx.isValid() || model_->isDir(idx)) return;  // a directory just expands
    emit fileActivated(model_->filePath(idx));
}

void FilesPane::onContextMenu(const QPoint& pos) {
    const QModelIndex idx = tree_->indexAt(pos);
    if (!idx.isValid()) return;

    QMenu menu(this);
    QAction* open = model_->isDir(idx) ? nullptr : menu.addAction(tr("Open"));
    QAction* rename = menu.addAction(tr("Rename…"));
    QAction* del = menu.addAction(tr("Delete"));
    QAction* hit = menu.exec(tree_->viewport()->mapToGlobal(pos));

    if (!hit) return;
    if (hit == open) emit fileActivated(model_->filePath(idx));
    else if (hit == rename) renameAt(idx);
    else if (hit == del) deleteAt(idx);
}

void FilesPane::renameAt(const QModelIndex& idx) {
    const QString path = model_->filePath(idx);
    const QFileInfo fi(path);
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename"), tr("New name:"),
                                               QLineEdit::Normal, fi.fileName(), &ok);
    if (!ok || name.isEmpty() || name == fi.fileName()) return;
    if (name.contains('/')) {
        emit statusMessage(tr("a name cannot contain '/'"));
        return;
    }
    const QString target = fi.absolutePath() + "/" + name;
    if (QFileInfo::exists(target)) {
        emit statusMessage(tr("%1 already exists").arg(name));
        return;
    }
    if (QFile::rename(path, target)) emit statusMessage(tr("renamed to %1").arg(name));
    else emit statusMessage(tr("could not rename %1").arg(fi.fileName()));
}

// Destructive, so it confirms — and a directory says so explicitly, because
// removeRecursively() takes its whole subtree with it.
void FilesPane::deleteAt(const QModelIndex& idx) {
    const QString path = model_->filePath(idx);
    const QFileInfo fi(path);
    const bool dir = fi.isDir();

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Delete"));
    box.setText(dir ? tr("Delete the folder %1 and everything in it?").arg(fi.fileName())
                    : tr("Delete %1?").arg(fi.fileName()));
    QPushButton* yes = box.addButton(tr("Delete"), QMessageBox::DestructiveRole);
    QPushButton* no = box.addButton(tr("Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(no);
    box.exec();
    if (box.clickedButton() != yes) return;

    const bool ok = dir ? QDir(path).removeRecursively() : QFile::remove(path);
    emit statusMessage(ok ? tr("deleted %1").arg(fi.fileName())
                          : tr("could not delete %1").arg(fi.fileName()));
}

}  // namespace odv
