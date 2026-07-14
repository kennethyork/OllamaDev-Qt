// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "TopologyPane.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVBoxLayout>

#include "Crew.h"
#include "Theme.h"

namespace odv {
namespace {

// A read-only live map of the running crew: the Director on top, one card per
// coder below, each coloured by state and captioned with its backend/model.
// Purely a viewer over Crew::boardState() (the same current.json the kanban
// polls); it changes nothing about how the crew runs.
//
// Simplified vs the PHP Topology: the desktop board file carries n/title/role/
// state/backend/model, so we draw exactly those. The PHP view also showed the
// per-coder branch, touched-file list and Auditor verdict — those are not in
// this build's board JSON, so they are deliberately omitted rather than faked.
class TopologyView : public QWidget {
public:
    explicit TopologyView(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(320, 240);
        timer_ = new QTimer(this);
        timer_->setInterval(1500);
        connect(timer_, &QTimer::timeout, this, [this] { poll(); });
    }

protected:
    void showEvent(QShowEvent* e) override {
        QWidget::showEvent(e);
        poll();
        timer_->start();
    }
    void hideEvent(QHideEvent* e) override {
        timer_->stop();
        QWidget::hideEvent(e);
    }

    void paintEvent(QPaintEvent*) override {
        const Theme::Colors c = Theme::currentColors();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), c.canvas);

        const QJsonArray subs = board_.value(QStringLiteral("subtasks")).toArray();
        const QString runId = board_.value(QStringLiteral("runId")).toString();
        if (runId.isEmpty() || subs.isEmpty()) {
            p.setPen(c.faint);
            p.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignCenter | Qt::TextWordWrap,
                       tr("No crew running.\nLaunch one —  Crew, or `ollamadev crew \"…\"` — and "
                          "the whole team appears here live: the Director and each coder, coloured "
                          "by state."));
            return;
        }

        const bool active = board_.value(QStringLiteral("active")).toBool();
        const QString task = board_.value(QStringLiteral("task")).toString();

        // ---- Director node, top-centre ----
        const int dirW = qMin(width() - 40, 420), dirH = 58;
        const QRectF dir((width() - dirW) / 2.0, 16, dirW, dirH);
        drawNode(p, dir, c.accent2,
                 QStringLiteral("Director  %1").arg(active ? tr("● live") : tr("○ done")),
                 task.isEmpty() ? tr("(waiting for a task)") : task, c);

        // ---- coder cards, wrapped grid below ----
        const int cardW = 190, cardH = 74, gap = 14;
        const int cols = qMax(1, (width() - gap) / (cardW + gap));
        const int gridW = qMin(subs.size(), cols) * (cardW + gap) - gap;
        const int x0 = (width() - gridW) / 2;
        const int y0 = dir.bottom() + 40;

        for (int i = 0; i < subs.size(); ++i) {
            const QJsonObject s = subs[i].toObject();
            const int row = i / cols, col = i % cols;
            const QRectF card(x0 + col * (cardW + gap), y0 + row * (cardH + gap), cardW, cardH);

            // Elbow connector from the Director's bottom edge to the card's top.
            p.setPen(QPen(c.border, 1.2));
            const QPointF a(dir.center().x(), dir.bottom());
            const QPointF b(card.center().x(), card.top());
            QPainterPath path(a);
            path.lineTo(a.x(), (a.y() + b.y()) / 2);
            path.lineTo(b.x(), (a.y() + b.y()) / 2);
            path.lineTo(b);
            p.drawPath(path);

            drawCoder(p, card, s, c);
        }
    }

private:
    void poll() {
        const QJsonObject b = Crew::boardState();
        const QByteArray sig = QJsonDocument(b).toJson(QJsonDocument::Compact);
        if (sig == sig_) return;  // avoid repainting a static board every 1.5s
        sig_ = sig;
        board_ = b;
        update();
    }

    static QColor stateColor(const QString& st, const Theme::Colors& c) {
        if (st == QLatin1String("doing")) return c.accent;
        if (st == QLatin1String("done")) return c.ok;
        if (st == QLatin1String("held")) return c.warn;
        if (st == QLatin1String("flagged")) return c.err;
        return c.faint;  // todo / unknown
    }
    static QString stateLabel(const QString& st) {
        if (st == QLatin1String("doing")) return QStringLiteral("● working");
        if (st == QLatin1String("done")) return QStringLiteral("✓ done");
        if (st == QLatin1String("held")) return QStringLiteral("⚠ held");
        if (st == QLatin1String("flagged")) return QStringLiteral("flagged");
        return QStringLiteral("○ queued");
    }

    void drawNode(QPainter& p, const QRectF& r, const QColor& accent, const QString& top,
                  const QString& sub, const Theme::Colors& c) {
        p.setBrush(c.bg2);
        p.setPen(QPen(accent, 2));
        p.drawRoundedRect(r, 8, 8);
        p.setPen(c.fg);
        QFont f = font();
        f.setBold(true);
        p.setFont(f);
        p.drawText(r.adjusted(12, 8, -12, -r.height() / 2),
                   Qt::AlignLeft | Qt::AlignVCenter, top);
        f.setBold(false);
        p.setFont(f);
        p.setPen(c.dim);
        p.drawText(r.adjusted(12, r.height() / 2 - 4, -12, -8), Qt::AlignLeft | Qt::AlignVCenter,
                   p.fontMetrics().elidedText(sub, Qt::ElideRight, r.width() - 24));
    }

    void drawCoder(QPainter& p, const QRectF& r, const QJsonObject& s, const Theme::Colors& c) {
        const QString st = s.value(QStringLiteral("state")).toString(QStringLiteral("todo"));
        const QColor accent = stateColor(st, c);
        p.setBrush(c.bg3);
        p.setPen(QPen(c.border, 1));
        p.drawRoundedRect(r, 6, 6);
        // State stripe down the left edge.
        p.fillRect(QRectF(r.left(), r.top() + 1, 3, r.height() - 2), accent);

        const int n = s.value(QStringLiteral("n")).toInt();
        const QString role = s.value(QStringLiteral("role")).toString(QStringLiteral("coder"));
        const QString title = s.value(QStringLiteral("title")).toString();
        const QString engine = QStringLiteral("%1 · %2")
                                   .arg(s.value(QStringLiteral("backend")).toString(QStringLiteral("ollama")),
                                        s.value(QStringLiteral("model")).toString(QStringLiteral("—")));

        QFont f = font();
        f.setBold(true);
        p.setFont(f);
        p.setPen(c.fg);
        p.drawText(r.adjusted(12, 7, -8, 0), Qt::AlignLeft | Qt::AlignTop,
                   p.fontMetrics().elidedText(QStringLiteral("#%1 %2").arg(n).arg(role),
                                              Qt::ElideRight, r.width() - 20));
        f.setBold(false);
        p.setFont(f);
        p.setPen(c.dim);
        p.drawText(r.adjusted(12, 26, -8, 0), Qt::AlignLeft | Qt::AlignTop,
                   p.fontMetrics().elidedText(title.isEmpty() ? engine : title, Qt::ElideRight,
                                              r.width() - 20));
        p.setPen(c.faint);
        p.drawText(r.adjusted(12, 42, -8, 0), Qt::AlignLeft | Qt::AlignTop,
                   p.fontMetrics().elidedText(engine, Qt::ElideRight, r.width() - 20));
        p.setPen(accent);
        p.drawText(r.adjusted(12, 0, -8, -6), Qt::AlignLeft | Qt::AlignBottom, stateLabel(st));
    }

    QTimer* timer_ = nullptr;
    QJsonObject board_;
    QByteArray sig_;
};

}  // namespace

PaneSpec makeTopologyPaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("topology");
    s.title = QStringLiteral("Crew topology");
    s.group = QStringLiteral("Crew");
    s.singleton = true;
    s.factory = [](PaneHost&) -> QWidget* { return new TopologyView; };
    return s;
}

}  // namespace odv
