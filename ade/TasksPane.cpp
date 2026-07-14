// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "TasksPane.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "Theme.h"

namespace odv {
namespace {

struct Card {
    QString id;
    QString title;
    QString col;  // "todo" | "doing" | "done"
};

// A manual, local kanban — the desktop's own scratch board, distinct from the
// crew board (which the engine owns). Cards are add/move/delete only and persist
// to ~/.ollamadev/desktop-tasks.json, a small file this pane fully owns, so a
// restart brings the board back. No engine coupling on purpose: this is the
// human's to-do list, not the Director's plan.
class TasksWidget : public QWidget {
public:
    explicit TasksWidget(QWidget* parent = nullptr) : QWidget(parent) {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(8, 8, 8, 8);
        root->setSpacing(8);

        auto* addBar = new QHBoxLayout;
        input_ = new QLineEdit(this);
        input_->setPlaceholderText(tr("New task — Enter to add to To-do"));
        auto* add = new QPushButton(tr("＋ Add"), this);
        add->setProperty("cta", true);
        addBar->addWidget(input_, 1);
        addBar->addWidget(add);
        root->addLayout(addBar);

        columns_ = new QWidget(this);
        colsLayout_ = new QHBoxLayout(columns_);
        colsLayout_->setContentsMargins(0, 0, 0, 0);
        colsLayout_->setSpacing(8);
        root->addWidget(columns_, 1);

        connect(add, &QPushButton::clicked, this, [this] { addCard(); });
        connect(input_, &QLineEdit::returnPressed, this, [this] { addCard(); });

        load();
        rebuild();
    }

private:
    static QString storePath() {
        return QDir::homePath() + QStringLiteral("/.ollamadev/desktop-tasks.json");
    }

    void load() {
        cards_.clear();
        QFile f(storePath());
        if (!f.open(QIODevice::ReadOnly)) return;
        const QJsonArray arr =
            QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("cards")).toArray();
        for (const QJsonValue& v : arr) {
            const QJsonObject o = v.toObject();
            Card c;
            c.id = o.value(QStringLiteral("id")).toString();
            c.title = o.value(QStringLiteral("title")).toString();
            c.col = o.value(QStringLiteral("col")).toString(QStringLiteral("todo"));
            if (!c.title.isEmpty()) cards_.push_back(c);
        }
    }

    void save() {
        QJsonArray arr;
        for (const Card& c : cards_)
            arr.append(QJsonObject{{"id", c.id}, {"title", c.title}, {"col", c.col}});
        QDir().mkpath(QFileInfo(storePath()).absolutePath());
        QFile f(storePath());
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(QJsonDocument(QJsonObject{{"cards", arr}}).toJson(QJsonDocument::Compact));
    }

    void addCard() {
        const QString t = input_->text().trimmed();
        if (t.isEmpty()) return;
        Card c;
        c.id = QString::number(QDateTime::currentMSecsSinceEpoch()) +
               QString::number(cards_.size());
        c.title = t;
        c.col = QStringLiteral("todo");
        cards_.push_back(c);
        input_->clear();
        save();
        rebuild();
    }

    void move(const QString& id, int dir) {
        static const QStringList order{QStringLiteral("todo"), QStringLiteral("doing"),
                                       QStringLiteral("done")};
        for (Card& c : cards_) {
            if (c.id != id) continue;
            int i = order.indexOf(c.col) + dir;
            i = qBound(0, i, order.size() - 1);
            c.col = order[i];
            break;
        }
        save();
        rebuild();
    }

    void remove(const QString& id) {
        for (int i = 0; i < cards_.size(); ++i)
            if (cards_[i].id == id) {
                cards_.remove(i);
                break;
            }
        save();
        rebuild();
    }

    void rebuild() {
        while (QLayoutItem* item = colsLayout_->takeAt(0)) {
            if (QWidget* w = item->widget()) w->deleteLater();
            delete item;
        }
        const QVector<QPair<QString, QString>> cols{{QStringLiteral("todo"), tr("To-do")},
                                                    {QStringLiteral("doing"), tr("Doing")},
                                                    {QStringLiteral("done"), tr("Done")}};
        for (const auto& col : cols) {
            int count = 0;
            for (const Card& c : cards_)
                if (c.col == col.first) ++count;
            colsLayout_->addWidget(makeColumn(col.first, col.second, count), 1);
        }
    }

    QWidget* makeColumn(const QString& key, const QString& label, int count) {
        const Theme::Colors c = Theme::currentColors();
        auto* col = new QFrame(columns_);
        col->setObjectName("taskCol");
        col->setStyleSheet(
            QStringLiteral("#taskCol{background:%1;border:1px solid %2;border-radius:6px;}")
                .arg(c.bg2.name(), c.border.name()));
        auto* v = new QVBoxLayout(col);
        v->setContentsMargins(6, 6, 6, 6);
        v->setSpacing(6);

        const QColor dot = key == "done" ? c.ok : key == "doing" ? c.accent : c.faint;
        v->addWidget(new QLabel(QStringLiteral("<span style='color:%1'>●</span> <b>%2</b> "
                                               "<span style='color:%3'>%4</span>")
                                    .arg(dot.name(), label.toHtmlEscaped(), c.faint.name(),
                                         QString::number(count)),
                                col));

        auto* scroll = new QScrollArea(col);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* body = new QWidget(scroll);
        auto* bl = new QVBoxLayout(body);
        bl->setContentsMargins(0, 0, 0, 0);
        bl->setSpacing(6);
        for (const Card& card : cards_)
            if (card.col == key) bl->addWidget(makeCard(card));
        bl->addStretch(1);
        scroll->setWidget(body);
        v->addWidget(scroll, 1);
        return col;
    }

    QWidget* makeCard(const Card& card) {
        const Theme::Colors c = Theme::currentColors();
        auto* frame = new QFrame;
        frame->setObjectName("taskCard");
        frame->setStyleSheet(QStringLiteral("#taskCard{background:%1;border:1px solid %2;"
                                            "border-radius:5px;}")
                                 .arg(c.bg3.name(), c.border.name()));
        auto* v = new QVBoxLayout(frame);
        v->setContentsMargins(8, 6, 8, 6);
        v->setSpacing(4);

        auto* title = new QLabel(card.title.toHtmlEscaped(), frame);
        title->setWordWrap(true);
        v->addWidget(title);

        auto* acts = new QHBoxLayout;
        acts->setSpacing(4);
        auto* left = new QPushButton(QStringLiteral("◀"), frame);
        auto* right = new QPushButton(QStringLiteral("▶"), frame);
        auto* del = new QPushButton(QStringLiteral("✕"), frame);
        del->setProperty("danger", true);
        for (QPushButton* b : {left, right, del}) b->setFixedWidth(30);
        left->setEnabled(card.col != QLatin1String("todo"));
        right->setEnabled(card.col != QLatin1String("done"));
        acts->addWidget(left);
        acts->addWidget(right);
        acts->addStretch(1);
        acts->addWidget(del);
        v->addLayout(acts);

        const QString id = card.id;
        connect(left, &QPushButton::clicked, this, [this, id] { move(id, -1); });
        connect(right, &QPushButton::clicked, this, [this, id] { move(id, +1); });
        connect(del, &QPushButton::clicked, this, [this, id] { remove(id); });
        return frame;
    }

    QLineEdit* input_ = nullptr;
    QWidget* columns_ = nullptr;
    QHBoxLayout* colsLayout_ = nullptr;
    QVector<Card> cards_;
};

}  // namespace

PaneSpec makeTasksPaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("tasks");
    s.title = QStringLiteral("▦ Tasks");
    s.group = QStringLiteral("Views");
    s.singleton = true;
    s.factory = [](PaneHost&) -> QWidget* { return new TasksWidget; };
    return s;
}

}  // namespace odv
