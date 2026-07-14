// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "CodeSearchPane.h"

#include <QApplication>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <thread>

#include "CodeIndex.h"
#include "Theme.h"

namespace odv {
namespace {

// Local semantic code search over CodeIndex. Both the query embed and the (much
// heavier) index build hit the local Ollama and block, so each runs on a detached
// worker thread and marshals its result back with QMetaObject::invokeMethod bound
// to `this` — if the pane is closed first, Qt drops the queued call, so there is
// no use-after-free and no need to own the thread.
class CodeSearchWidget : public QWidget {
public:
    explicit CodeSearchWidget(PaneHost& host, QWidget* parent = nullptr)
        : QWidget(parent), host_(host) {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(8, 8, 8, 8);
        root->setSpacing(8);

        auto* bar = new QHBoxLayout;
        query_ = new QLineEdit(this);
        query_->setPlaceholderText(tr("Search the repo by meaning — e.g. \"where is the auth token refreshed\""));
        searchBtn_ = new QPushButton(tr("Search"), this);
        searchBtn_->setProperty("cta", true);
        buildBtn_ = new QPushButton(tr("Build index"), this);
        bar->addWidget(query_, 1);
        bar->addWidget(searchBtn_);
        bar->addWidget(buildBtn_);
        root->addLayout(bar);

        status_ = new QLabel(this);
        status_->setStyleSheet(QStringLiteral("color:%1;").arg(Theme::currentColors().faint.name()));
        root->addWidget(status_);

        results_ = new QListWidget(this);
        results_->setWordWrap(true);
        results_->setAlternatingRowColors(true);
        root->addWidget(results_, 1);

        connect(query_, &QLineEdit::returnPressed, this, [this] { runSearch(); });
        connect(searchBtn_, &QPushButton::clicked, this, [this] { runSearch(); });
        connect(buildBtn_, &QPushButton::clicked, this, [this] { runBuild(); });
        connect(results_, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) { open(it); });
        connect(results_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) { open(it); });

        refreshStatus();
    }

private:
    void refreshStatus() {
        const IndexStatus st = CodeIndex::status();
        if (st.exists) {
            status_->setText(tr("Index ready · %1 files · %2 chunks · %3")
                                 .arg(st.files).arg(st.chunks).arg(st.model));
            buildBtn_->setText(tr("Rebuild index"));
        } else {
            status_->setText(tr("No index yet — click Build index (embeds the repo on your local Ollama)."));
            buildBtn_->setText(tr("Build index"));
        }
    }

    void setBusy(bool busy, const QString& msg) {
        query_->setEnabled(!busy);
        searchBtn_->setEnabled(!busy);
        buildBtn_->setEnabled(!busy);
        if (!msg.isEmpty()) status_->setText(msg);
    }

    void runSearch() {
        const QString q = query_->text().trimmed();
        if (q.isEmpty()) return;
        if (!CodeIndex::status().exists) {
            status_->setText(tr("No index yet — build it first."));
            return;
        }
        setBusy(true, tr("Searching…"));
        results_->clear();
        QPointer<CodeSearchWidget> self = this;
        std::thread([self, q] {
            SearchReport rep = CodeIndex::search(q, 20);
            QMetaObject::invokeMethod(qApp, [self, rep] {
                if (self) self->showResults(rep);
            });
        }).detach();
    }

    void showResults(const SearchReport& rep) {
        setBusy(false, QString());
        if (!rep.ok) {
            status_->setText(rep.error == QLatin1String("no_index")
                                 ? tr("No index — build it first.")
                                 : tr("Search failed: %1").arg(rep.error));
            refreshStatus();
            return;
        }
        status_->setText(tr("%1 result%2").arg(rep.hits.size()).arg(rep.hits.size() == 1 ? "" : "s"));
        for (const IndexHit& h : rep.hits) {
            auto* it = new QListWidgetItem(results_);
            const QString head = QStringLiteral("%1  ·  L%2-%3  ·  score %4")
                                     .arg(h.file)
                                     .arg(h.start)
                                     .arg(h.end)
                                     .arg(QString::number(h.score, 'f', 3));
            QString snip = h.snippet.trimmed();
            snip.replace('\t', QStringLiteral("    "));
            const QStringList lines = snip.split('\n');
            if (lines.size() > 3) snip = QStringList(lines.mid(0, 3)).join('\n') + QStringLiteral(" …");
            it->setText(head + "\n" + snip);
            it->setData(Qt::UserRole, h.file);  // repo-relative; resolved on open
        }
    }

    void open(QListWidgetItem* it) {
        if (!it) return;
        const QString rel = it->data(Qt::UserRole).toString();
        if (rel.isEmpty()) return;
        // CodeIndex hits are repo-relative; the editor opens an absolute path.
        host_.openFile(QDir(host_.project()).filePath(rel));
    }

    void runBuild() {
        setBusy(true, tr("Building index… embedding the repo on your local Ollama."));
        results_->clear();
        QPointer<CodeSearchWidget> self = this;
        std::thread([self] {
            BuildReport rep = CodeIndex::build([self](const QString& file, int done, int total) {
                if ((done & 15) != 0 && done != total) return;  // throttle UI churn
                QMetaObject::invokeMethod(qApp, [self, file, done, total] {
                    if (self)
                        self->status_->setText(
                            tr("Building index… %1/%2  %3").arg(done).arg(total).arg(file));
                });
            });
            QMetaObject::invokeMethod(qApp, [self, rep] {
                if (!self) return;
                self->setBusy(false, QString());
                if (rep.ok)
                    self->status_->setText(tr("Index built · %1 files · %2 chunks%3")
                                               .arg(rep.files).arg(rep.chunks)
                                               .arg(rep.skipped ? tr(" · %1 skipped").arg(rep.skipped)
                                                                : QString()));
                else
                    self->status_->setText(tr("Build failed: %1").arg(
                        rep.error.isEmpty() ? tr("unknown error") : rep.error));
                self->refreshStatus();
            });
        }).detach();
    }

    PaneHost& host_;
    QLineEdit* query_ = nullptr;
    QPushButton* searchBtn_ = nullptr;
    QPushButton* buildBtn_ = nullptr;
    QLabel* status_ = nullptr;
    QListWidget* results_ = nullptr;
};

}  // namespace

PaneSpec makeCodeSearchPaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("codesearch");
    s.title = QStringLiteral("Code search");
    s.group = QStringLiteral("Tools");
    s.singleton = true;
    s.factory = [](PaneHost& host) -> QWidget* { return new CodeSearchWidget(host); };
    return s;
}

}  // namespace odv
