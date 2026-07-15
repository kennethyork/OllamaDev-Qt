// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Pane.h"

#include <QAbstractButton>
#include <QApplication>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSceneResizeEvent>
#include <QGraphicsView>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#include "Theme.h"

namespace odv {

Pane::Pane(const QString& id, const QString& title, QWidget* content, QGraphicsItem* parent)
    : QGraphicsWidget(parent), id_(id), title_(title), content_(content) {
    setFlag(QGraphicsItem::ItemIsFocusable, true);
    setAcceptHoverEvents(true);
    setMinimumSize(kMinW, kMinH);

    if (content_) {
        proxy_ = new QGraphicsProxyWidget(this);
        // The proxy must not paint a window frame of its own — the item draws it.
        content_->setAttribute(Qt::WA_TranslucentBackground, false);
        proxy_->setWidget(content_);
        proxy_->setFocusPolicy(Qt::StrongFocus);
    }
    resize(540, 340);
    layoutContent();
}

void Pane::setTitle(const QString& t) {
    title_ = t;
    update();
}

void Pane::setGeometryF(const QRectF& r) {
    QRectF g = r;
    if (g.width() < kMinW) g.setWidth(kMinW);
    if (g.height() < kMinH) g.setHeight(kMinH);
    setGeometry(g);
    layoutContent();
}

void Pane::setMaximised(bool on) {
    maximised_ = on;
    update();
}

QRectF Pane::headRect() const { return QRectF(0, 0, size().width(), kHeadH); }

QRectF Pane::closeRect() const {
    return QRectF(size().width() - 22, (kHeadH - 18) / 2, 18, 18);
}

QRectF Pane::zoomRect() const {
    return QRectF(size().width() - 44, (kHeadH - 18) / 2, 18, 18);
}

QRectF Pane::gripRect() const {
    return QRectF(size().width() - kGrip, size().height() - kGrip, kGrip, kGrip);
}

void Pane::layoutContent() {
    if (!proxy_) return;
    const qreal w = size().width(), h = size().height();
    // Leave a kEdge margin on the right and bottom so that border belongs to the
    // pane, not the content proxy — otherwise the content grabs the mouse press and
    // the resize grip/edges never fire.
    proxy_->setGeometry(
        QRectF(1, kHeadH, qMax(0.0, w - 1 - kEdge), qMax(0.0, h - kHeadH - kEdge)));
}

int Pane::resizeEdgeAt(const QPointF& pos) const {
    if (pos.y() <= kHeadH) return 0;  // the header is a move zone, never a resize one
    if (gripRect().contains(pos)) return 3;  // the corner grip resizes both (diagonal)
    int e = 0;
    if (pos.x() >= size().width() - kEdge) e |= 1;   // right edge
    if (pos.y() >= size().height() - kEdge) e |= 2;  // bottom edge
    return e;
}

void Pane::resizeEvent(QGraphicsSceneResizeEvent* e) {
    QGraphicsWidget::resizeEvent(e);
    layoutContent();
}

void Pane::paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) {
    const Theme::Colors c = Theme::currentColors();
    const QRectF r = QRectF(QPointF(0, 0), size());
    const bool active = hasFocus() || (proxy_ && proxy_->hasFocus());

    p->setRenderHint(QPainter::Antialiasing, true);

    QPainterPath frame;
    frame.addRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
    p->fillPath(frame, c.bg);

    // Header strip, clipped to the rounded frame so the top corners stay round.
    p->save();
    p->setClipPath(frame);
    p->fillRect(headRect(), c.bg2);
    p->setPen(QPen(c.border, 1));
    p->drawLine(QPointF(0, kHeadH), QPointF(r.width(), kHeadH));
    p->restore();

    p->setPen(QPen(active ? c.accent : c.border, active ? 1.4 : 1.0));
    p->drawPath(frame);

    QFont f = QApplication::font();
    f.setPointSizeF(qMax(7.0, f.pointSizeF() - 0.5));
    f.setBold(true);
    p->setFont(f);
    p->setPen(c.fg);
    const QRectF titleR(9, 0, qMax(0.0, r.width() - 56), kHeadH);
    p->drawText(titleR, Qt::AlignVCenter | Qt::AlignLeft,
                p->fontMetrics().elidedText(title_, Qt::ElideRight, int(titleR.width())));

    // ⤢ focus toggle and ✕ close. Drawn as glyphs, hit-tested as rects.
    f.setBold(false);
    f.setPointSizeF(f.pointSizeF() + 1.0);
    p->setFont(f);
    p->setPen(hover_ == 2 ? c.accent : c.faint);
    p->drawText(zoomRect(), Qt::AlignCenter, maximised_ ? QStringLiteral("⤡") : QStringLiteral("⤢"));
    p->setPen(hover_ == 1 ? c.err : c.faint);
    p->drawText(closeRect(), Qt::AlignCenter, QStringLiteral("✕"));

    // Resize grip: two hatch strokes in the bottom-right corner.
    const QRectF g = gripRect();
    p->setPen(QPen(hover_ == 3 ? c.accent : c.border, 1.4));
    p->drawLine(g.topRight() + QPointF(-2, 6), g.bottomRight() + QPointF(-6, -2));
    p->drawLine(g.topRight() + QPointF(-2, 11), g.bottomRight() + QPointF(-11, -2));
}

void Pane::mousePressEvent(QGraphicsSceneMouseEvent* e) {
    emit raised();
    if (e->button() != Qt::LeftButton) {
        QGraphicsWidget::mousePressEvent(e);
        return;
    }
    const QPointF pos = e->pos();

    if (closeRect().contains(pos)) {
        e->accept();
        askThenClose();
        return;
    }
    if (zoomRect().contains(pos)) {
        e->accept();
        emit focusToggleRequested();
        return;
    }
    if (int edges = resizeEdgeAt(pos)) {
        drag_ = Drag::Resize;
        resizeEdges_ = edges;
        dragOrigin_ = e->scenePos();
        dragGeom_ = geometry();
        e->accept();
        return;
    }
    if (headRect().contains(pos)) {
        drag_ = Drag::Move;
        dragOrigin_ = e->scenePos();
        dragGeom_ = geometry();
        e->accept();
        return;
    }
    QGraphicsWidget::mousePressEvent(e);
}

void Pane::mouseMoveEvent(QGraphicsSceneMouseEvent* e) {
    if (drag_ == Drag::None) {
        QGraphicsWidget::mouseMoveEvent(e);
        return;
    }
    // Scene deltas ARE world deltas — the view transform has already divided out
    // the zoom, so the pane tracks the cursor 1:1 at any scale.
    const QPointF d = e->scenePos() - dragOrigin_;
    if (drag_ == Drag::Move) {
        setGeometry(QRectF(dragGeom_.topLeft() + d, dragGeom_.size()));
    } else {
        const qreal nw = (resizeEdges_ & 1) ? dragGeom_.width() + d.x() : dragGeom_.width();
        const qreal nh = (resizeEdges_ & 2) ? dragGeom_.height() + d.y() : dragGeom_.height();
        setGeometryF(QRectF(dragGeom_.topLeft(), QSizeF(nw, nh)));
    }
    e->accept();
}

void Pane::mouseReleaseEvent(QGraphicsSceneMouseEvent* e) {
    if (drag_ != Drag::None) {
        drag_ = Drag::None;
        emit geometryEdited();
        e->accept();
        return;
    }
    QGraphicsWidget::mouseReleaseEvent(e);
}

void Pane::mouseDoubleClickEvent(QGraphicsSceneMouseEvent* e) {
    if (headRect().contains(e->pos()) && !closeRect().contains(e->pos()) &&
        !zoomRect().contains(e->pos())) {
        emit focusToggleRequested();
        e->accept();
        return;
    }
    QGraphicsWidget::mouseDoubleClickEvent(e);
}

void Pane::hoverMoveEvent(QGraphicsSceneHoverEvent* e) {
    const QPointF pos = e->pos();
    const int was = hover_;
    const int edges = resizeEdgeAt(pos);
    hover_ = closeRect().contains(pos)  ? 1
             : zoomRect().contains(pos) ? 2
             : edges                    ? 3
                                        : 0;
    if (edges == 3) setCursor(Qt::SizeFDiagCursor);
    else if (edges == 1) setCursor(Qt::SizeHorCursor);
    else if (edges == 2) setCursor(Qt::SizeVerCursor);
    else if (headRect().contains(pos)) setCursor(Qt::SizeAllCursor);
    else setCursor(Qt::ArrowCursor);
    if (hover_ != was) update();
    QGraphicsWidget::hoverMoveEvent(e);
}

void Pane::hoverLeaveEvent(QGraphicsSceneHoverEvent* e) {
    hover_ = 0;
    unsetCursor();
    update();
    QGraphicsWidget::hoverLeaveEvent(e);
}

// The X ALWAYS confirms. This is a product requirement carried over from the PHP
// app (App.confirmAction on every pane's ✕) — closing a pane can kill a live PTY
// or drop an unsaved buffer, so it is never a one-click action. It lives HERE,
// not in MainWindow, so no future call site can route around it.
//
// Deferred to the next event-loop turn: opening a modal from inside a graphics
// mouse handler would run a nested loop while the scene still holds the mouse
// grab, which leaves the item stuck in a pressed state.
void Pane::askThenClose() {
    if (closing_) return;
    closing_ = true;
    QTimer::singleShot(0, this, [this] {
        QWidget* parent = scene() && !scene()->views().isEmpty() ? scene()->views().first()
                                                                 : nullptr;
        QMessageBox box(parent);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(tr("Close window"));
        box.setText(tr("Close the %1 window?").arg(title_));
        QPushButton* yes = box.addButton(tr("Close"), QMessageBox::AcceptRole);
        box.addButton(tr("Cancel"), QMessageBox::RejectRole);
        box.setDefaultButton(yes);
        box.exec();
        closing_ = false;
        if (box.clickedButton() == yes) emit closeRequested();
    });
}

}  // namespace odv
