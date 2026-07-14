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
#include <QGraphicsView>
#include <QHash>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>

class QGraphicsScene;
class QLabel;
class QWidget;

namespace odv {

class Pane;
class Minimap;

// The infinite canvas: a pannable, zoomable surface holding floating windows.
// Port of renderFree / bindCanvas / setZoom / wireFree from the PHP app.js.
//
// QGraphicsView rather than a hand-transformed QWidget: it already owns the
// world<->view mapping, hit-testing under a transform, and z-ordering — all
// three of which app.js had to reimplement (dividing drag deltas by the zoom,
// tracking `_zTop` by hand, re-anchoring pan on every zoom).
//
// "Infinite" is a very large fixed sceneRect rather than a truly unbounded one:
// QGraphicsView derives its scroll range from the scene rect, and an empty rect
// makes the view resize itself to the item bounding box — which would drag the
// viewport around as panes move. ±1e6 world units at the 0.2 zoom floor is far
// more canvas than any session can traverse.
class Canvas : public QGraphicsView {
    Q_OBJECT

public:
    static constexpr double kMinZoom = 0.2;
    static constexpr double kMaxZoom = 3.0;
    static constexpr double kGrid = 26.0;  // dot lattice pitch, as in app.css

    explicit Canvas(QWidget* parent = nullptr);

    Pane* addPane(const QString& id, const QString& title, QWidget* content,
                  const QRectF& geom = QRectF());
    void removePane(Pane* p);
    Pane* pane(const QString& id) const;
    QList<Pane*> panes() const;
    int paneCount() const;

    void raisePane(Pane* p);
    void toggleFocus(Pane* p);  // maximise to the viewport / restore
    QString maximisedId() const { return maxId_; }

    double zoom() const;
    void setZoom(double z, const QPoint& anchor = QPoint(-1, -1));
    void zoomBy(double factor, const QPoint& anchor = QPoint(-1, -1));

    QPointF viewOrigin() const;              // world point at the viewport's top-left
    void setView(const QPointF& origin, double zoom);
    QRectF viewportWorldRect() const;
    QPointF worldAt(const QPoint& viewportPos) const;

    void centerAll();  // fit every pane in view

    // Cascade a new pane into the CURRENTLY VISIBLE area, so it can never spawn
    // off-screen when the canvas is panned far from the origin.
    QRectF nextSpawnGeom(const QSizeF& size);

    void refreshTheme();

    // The overview map, bottom-left. An infinite canvas has one failure mode — you
    // pan away, lose every pane off-screen, and cannot tell which direction they
    // are in. Toggleable, because on a small canvas it is just clutter.
    void setMinimapVisible(bool on);
    bool minimapVisible() const;

signals:
    void paneClosed(const QString& id);
    void contextMenuRequestedAt(const QPoint& globalPos, const QPointF& worldPos);
    void viewChanged();

protected:
    void drawBackground(QPainter* p, const QRectF& rect) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void buildZoomControl();
    void placeZoomControl();
    void syncZoomLabel();

    QGraphicsScene* scene_ = nullptr;
    QHash<QString, Pane*> panes_;

    bool panning_ = false;
    QPoint panLast_;
    Qt::CursorShape restoreCursor_ = Qt::ArrowCursor;

    qreal zTop_ = 1;
    int spawnSeq_ = 0;

    QString maxId_;        // pane currently maximised, if any
    QRectF maxSaved_;      // its pre-maximise geometry

    QWidget* zoomCtl_ = nullptr;

    Minimap* minimap_ = nullptr;
    QLabel* zoomLabel_ = nullptr;
};

}  // namespace odv
