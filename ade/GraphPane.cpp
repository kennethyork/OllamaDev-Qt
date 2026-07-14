// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "GraphPane.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>
#include <cmath>

#include "Memory.h"
#include "Theme.h"

namespace odv {
namespace {

// One node in the laid-out graph. Positions live in an abstract unit space that
// paintEvent fits into the current widget rect, so the layout survives resizes
// without recomputing the (relatively expensive) force simulation.
struct GNode {
    QString id, title;
    int degree = 0;
    QPointF pos;   // unit space, roughly [-1,1]
    QPointF disp;  // scratch for the force step
};

// A node-link view of the memory graph, drawn with QPainter — no graph library,
// which is the whole point of the zero-dep rule. The layout is a small
// Fruchterman-Reingold spring model: linked notes attract, every pair repels, so
// clusters of related notes fall out visually. It runs a fixed number of
// iterations once per rebuild (memory graphs are tens of nodes, not thousands),
// then we only ever remap the cached positions into the widget.
class GraphView : public QWidget {
public:
    explicit GraphView(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(240, 200);
    }

    std::function<void(const QString& id)> onSelect;

    void setData(const QJsonObject& g) {
        nodes_.clear();
        edges_.clear();
        index_.clear();
        selected_.clear();

        const QJsonArray ns = g.value(QStringLiteral("nodes")).toArray();
        for (const QJsonValue& v : ns) {
            const QJsonObject o = v.toObject();
            GNode n;
            n.id = o.value(QStringLiteral("id")).toString();
            n.title = o.value(QStringLiteral("title")).toString();
            n.degree = o.value(QStringLiteral("degree")).toInt();
            index_.insert(n.id, nodes_.size());
            nodes_.push_back(n);
        }
        const QJsonArray es = g.value(QStringLiteral("edges")).toArray();
        for (const QJsonValue& v : es) {
            const QJsonObject o = v.toObject();
            const int a = index_.value(o.value(QStringLiteral("from")).toString(), -1);
            const int b = index_.value(o.value(QStringLiteral("to")).toString(), -1);
            if (a >= 0 && b >= 0 && a != b) edges_.push_back({a, b});
        }
        layout();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const Theme::Colors c = Theme::currentColors();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), c.canvas);

        if (nodes_.isEmpty()) {
            p.setPen(c.faint);
            p.drawText(rect().adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap,
                       tr("No memory notes yet.\nWrite one — the agent's `remember` tool, or "
                          "`ollamadev memory new` — and the wiki-linked graph appears here."));
            return;
        }

        p.setPen(QPen(c.border, 1.2));
        for (const auto& e : edges_)
            p.drawLine(mapPoint(nodes_[e.first].pos), mapPoint(nodes_[e.second].pos));

        for (const GNode& n : nodes_) {
            const QPointF pt = mapPoint(n.pos);
            const qreal r = nodeRadius(n);
            const bool sel = n.id == selected_;
            p.setBrush(sel ? c.accent : c.accent2);
            p.setPen(QPen(sel ? c.fg : c.border, sel ? 2.0 : 1.0));
            p.drawEllipse(pt, r, r);

            p.setPen(sel ? c.fg : c.dim);
            const QString label = n.title.isEmpty() ? n.id : n.title;
            p.drawText(QRectF(pt.x() - 70, pt.y() + r + 2, 140, 16),
                       Qt::AlignHCenter | Qt::AlignTop,
                       p.fontMetrics().elidedText(label, Qt::ElideRight, 140));
        }
    }

    void mousePressEvent(QMouseEvent* e) override {
        const int hit = nodeAt(e->pos());
        if (hit < 0) return;
        selected_ = nodes_[hit].id;
        update();
        if (onSelect) onSelect(selected_);
    }

    void resizeEvent(QResizeEvent* e) override {
        QWidget::resizeEvent(e);
        update();  // remap only — layout positions are resolution-independent
    }

private:
    qreal nodeRadius(const GNode& n) const { return 6.0 + qMin(n.degree, 8) * 1.6; }

    // Fit unit-space positions into the widget with a margin, one scale for both
    // axes so the layout keeps its shape.
    QPointF mapPoint(const QPointF& u) const {
        const qreal m = 44;
        const qreal s = qMax(1.0, qMin(width(), height()) - 2 * m) / 2.0;
        return QPointF(width() / 2.0 + u.x() * s, height() / 2.0 + u.y() * s);
    }

    int nodeAt(const QPointF& widgetPt) const {
        for (int i = 0; i < nodes_.size(); ++i)
            if (QLineF(widgetPt, mapPoint(nodes_[i].pos)).length() <= nodeRadius(nodes_[i]) + 4)
                return i;
        return -1;
    }

    void layout() {
        const int n = nodes_.size();
        if (n == 0) return;
        if (n == 1) {
            nodes_[0].pos = QPointF(0, 0);
            return;
        }
        // Deterministic circular seed → stable layout run-to-run.
        for (int i = 0; i < n; ++i) {
            const double a = 2.0 * M_PI * i / n;
            nodes_[i].pos = QPointF(0.9 * std::cos(a), 0.9 * std::sin(a));
        }
        const double k = std::sqrt(4.0 / n);  // ideal edge length in ~[-1,1]^2
        double temp = 0.9;                     // max step, cooled each iteration
        for (int it = 0; it < 220; ++it) {
            for (auto& v : nodes_) v.disp = QPointF(0, 0);
            for (int i = 0; i < n; ++i) {
                for (int j = i + 1; j < n; ++j) {
                    QPointF d = nodes_[i].pos - nodes_[j].pos;
                    double dist = std::hypot(d.x(), d.y());
                    if (dist < 1e-4) {
                        d = QPointF((i % 3 - 1) * 1e-3, (j % 3 - 1) * 1e-3);
                        dist = std::hypot(d.x(), d.y()) + 1e-4;
                    }
                    const QPointF push = d / dist * (k * k / dist);
                    nodes_[i].disp += push;
                    nodes_[j].disp -= push;
                }
            }
            for (const auto& e : edges_) {
                QPointF d = nodes_[e.first].pos - nodes_[e.second].pos;
                const double dist = std::hypot(d.x(), d.y()) + 1e-4;
                const QPointF pull = d / dist * (dist * dist / k);
                nodes_[e.first].disp -= pull;
                nodes_[e.second].disp += pull;
            }
            for (auto& v : nodes_) {
                const double len = std::hypot(v.disp.x(), v.disp.y());
                if (len > 1e-6) v.pos += v.disp / len * std::min(len, temp);
                v.pos.setX(std::clamp(v.pos.x(), -1.0, 1.0));
                v.pos.setY(std::clamp(v.pos.y(), -1.0, 1.0));
            }
            temp *= 0.97;
        }
        // Normalise to fill the box wherever the sim settled.
        double minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
        for (const auto& v : nodes_) {
            minx = std::min(minx, v.pos.x());
            maxx = std::max(maxx, v.pos.x());
            miny = std::min(miny, v.pos.y());
            maxy = std::max(maxy, v.pos.y());
        }
        const double s = std::min((maxx - minx) > 1e-6 ? 1.8 / (maxx - minx) : 1.0,
                                  (maxy - miny) > 1e-6 ? 1.8 / (maxy - miny) : 1.0);
        for (auto& v : nodes_) {
            v.pos.setX((v.pos.x() - (minx + maxx) / 2) * s);
            v.pos.setY((v.pos.y() - (miny + maxy) / 2) * s);
        }
    }

    QVector<GNode> nodes_;
    QVector<QPair<int, int>> edges_;
    QHash<QString, int> index_;
    QString selected_;
};

// The pane: graph on the left, the clicked note's body on the right.
class GraphPaneWidget : public QWidget {
public:
    explicit GraphPaneWidget(PaneHost& host, QWidget* parent = nullptr)
        : QWidget(parent), host_(host) {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(8, 8, 8, 8);
        root->setSpacing(8);

        auto* bar = new QHBoxLayout;
        auto* refresh = new QPushButton(tr("↻ Refresh"), this);
        count_ = new QLabel(this);
        bar->addWidget(refresh);
        bar->addWidget(count_, 1);
        root->addLayout(bar);

        auto* split = new QHBoxLayout;
        split->setSpacing(8);
        view_ = new GraphView(this);
        detail_ = new QTextBrowser(this);
        detail_->setOpenExternalLinks(false);
        detail_->setMaximumWidth(360);
        detail_->setPlaceholderText(tr("Click a node to read the note."));
        split->addWidget(view_, 3);
        split->addWidget(detail_, 2);
        root->addLayout(split, 1);

        view_->onSelect = [this](const QString& id) { showNote(id); };
        connect(refresh, &QPushButton::clicked, this, [this] { reload(); });
        reload();
    }

private:
    void reload() {
        view_->setData(Memory::graph());
        const int n = Memory::all().size();
        count_->setText(tr("%1 note%2").arg(n).arg(n == 1 ? "" : "s"));
        host_.setStatus(tr("memory graph — %1 notes").arg(n));
    }

    void showNote(const QString& id) {
        const MemoryNote note = Memory::get(id);
        if (note.isNull()) {
            detail_->setPlainText(tr("(note not found: %1)").arg(id));
            return;
        }
        // The body is markdown; QTextBrowser renders a useful subset directly,
        // enough for a reader panel without pulling in a real Markdown parser.
        detail_->setMarkdown("# " + note.title + "\n\n" +
                             (note.tags.isEmpty() ? QString()
                                                  : "`#" + note.tags.join("` `#") + "`\n\n") +
                             note.body);
        host_.setStatus(note.tags.isEmpty() ? note.title
                                            : tr("%1 · #%2").arg(note.title, note.tags.join(", ")));
    }

    PaneHost& host_;
    GraphView* view_ = nullptr;
    QTextBrowser* detail_ = nullptr;
    QLabel* count_ = nullptr;
};

}  // namespace

PaneSpec makeGraphPaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("graph");
    s.title = QStringLiteral("Memory graph");
    s.group = QStringLiteral("Views");
    s.singleton = true;
    s.factory = [](PaneHost& host) -> QWidget* { return new GraphPaneWidget(host); };
    return s;
}

}  // namespace odv
