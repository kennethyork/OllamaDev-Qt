#include "Canvas.h"

#include <QContextMenuEvent>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QWheelEvent>
#include <cmath>
#include <utility>

#include "Pane.h"
#include "Theme.h"

namespace odv {

Canvas::Canvas(QWidget* parent) : QGraphicsView(parent) {
    scene_ = new QGraphicsScene(this);
    scene_->setSceneRect(-1.0e6, -1.0e6, 2.0e6, 2.0e6);
    // BspTree indexing assumes a mostly-static scene; panes move constantly and
    // there are tens of them, not thousands, so linear hit-testing is cheaper and
    // never leaves stale index entries behind a drag.
    scene_->setItemIndexMethod(QGraphicsScene::NoIndex);
    setScene(scene_);

    setRenderHint(QPainter::Antialiasing, true);
    setRenderHint(QPainter::TextAntialiasing, true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setTransformationAnchor(QGraphicsView::NoAnchor);  // zoom-to-cursor is done by hand
    setResizeAnchor(QGraphicsView::NoAnchor);
    setDragMode(QGraphicsView::NoDrag);
    setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);

    buildZoomControl();
    setView(QPointF(0, 0), 1.0);
}

// ---- panes ----------------------------------------------------------------

Pane* Canvas::addPane(const QString& id, const QString& title, QWidget* content,
                      const QRectF& geom) {
    if (Pane* existing = pane(id)) return existing;  // singleton views re-focus

    auto* p = new Pane(id, title, content);
    scene_->addItem(p);
    p->setGeometryF(geom.isValid() && !geom.isEmpty()
                        ? geom
                        : nextSpawnGeom(QSizeF(540, 340)));
    p->setZValue(++zTop_);
    panes_.insert(id, p);

    connect(p, &Pane::raised, this, [this, p] { raisePane(p); });
    connect(p, &Pane::focusToggleRequested, this, [this, p] { toggleFocus(p); });
    connect(p, &Pane::geometryEdited, this, &Canvas::viewChanged);
    connect(p, &Pane::closeRequested, this, [this, p] {
        const QString pid = p->id();
        removePane(p);
        emit paneClosed(pid);
    });

    if (p->content()) p->content()->setFocus();
    return p;
}

void Canvas::removePane(Pane* p) {
    if (!p) return;
    panes_.remove(p->id());
    if (maxId_ == p->id()) maxId_.clear();
    scene_->removeItem(p);
    p->deleteLater();  // may be called from the pane's own signal
    emit viewChanged();
}

Pane* Canvas::pane(const QString& id) const { return panes_.value(id, nullptr); }

QList<Pane*> Canvas::panes() const { return panes_.values(); }

int Canvas::paneCount() const { return panes_.size(); }

void Canvas::raisePane(Pane* p) {
    if (!p) return;
    p->setZValue(++zTop_);
    if (p->content()) p->content()->setFocus();
}

// Focus/maximise: the pane fills the visible canvas, exactly like tiled zoom in
// the original. Its pre-maximise geometry is remembered, never overwritten, so
// toggling off restores the free layout untouched.
void Canvas::toggleFocus(Pane* p) {
    if (!p) return;
    if (maxId_ == p->id()) {
        p->setMaximised(false);
        p->setGeometryF(maxSaved_);
        maxId_.clear();
        emit viewChanged();
        return;
    }
    if (Pane* prev = pane(maxId_)) {
        prev->setMaximised(false);
        prev->setGeometryF(maxSaved_);
    }
    maxId_ = p->id();
    maxSaved_ = p->geometryF();
    p->setMaximised(true);
    p->setGeometryF(viewportWorldRect().adjusted(12, 12, -12, -12));
    raisePane(p);
    emit viewChanged();
}

// ---- view -----------------------------------------------------------------

double Canvas::zoom() const { return transform().m11(); }

void Canvas::setZoom(double z, const QPoint& anchor) {
    z = qBound(kMinZoom, z, kMaxZoom);
    const bool useAnchor = anchor.x() >= 0 && anchor.y() >= 0;
    const QPointF before = useAnchor ? mapToScene(anchor) : QPointF();

    QTransform t;
    t.scale(z, z);
    setTransform(t);

    if (useAnchor) {
        // Re-anchor so the world point under the cursor did not move — the same
        // trick setZoom() in app.js pulled with panX/panY.
        const QPointF after = mapToScene(anchor);
        const QPointF c = viewportWorldRect().center();
        centerOn(c + (before - after));
    }
    syncZoomLabel();
    emit viewChanged();
}

void Canvas::zoomBy(double factor, const QPoint& anchor) { setZoom(zoom() * factor, anchor); }

QPointF Canvas::viewOrigin() const { return mapToScene(QPoint(0, 0)); }

QRectF Canvas::viewportWorldRect() const { return mapToScene(viewport()->rect()).boundingRect(); }

QPointF Canvas::worldAt(const QPoint& viewportPos) const { return mapToScene(viewportPos); }

void Canvas::setView(const QPointF& origin, double z) {
    setZoom(z);
    const QRectF vr = viewportWorldRect();
    centerOn(origin.x() + vr.width() / 2.0, origin.y() + vr.height() / 2.0);
    syncZoomLabel();
    emit viewChanged();
}

void Canvas::centerAll() {
    if (panes_.isEmpty()) {
        setView(QPointF(0, 0), 1.0);
        return;
    }
    QRectF box;
    for (Pane* p : std::as_const(panes_)) box = box.isNull() ? p->geometryF() : box.united(p->geometryF());
    box.adjust(-40, -40, 40, 40);

    const QSizeF vp = viewport()->rect().size();
    const double z = qBound(kMinZoom,
                            qMin(vp.width() / qMax(1.0, box.width()),
                                 vp.height() / qMax(1.0, box.height())),
                            kMaxZoom);
    setZoom(z);
    centerOn(box.center());
    syncZoomLabel();
    emit viewChanged();
}

QRectF Canvas::nextSpawnGeom(const QSizeF& size) {
    const QRectF vp = viewportWorldRect();
    // Never bigger than the visible area, never smaller than a pane's floor.
    const double w = qBound(double(Pane::kMinW), size.width(), qMax(double(Pane::kMinW), vp.width() - 48));
    const double h = qBound(double(Pane::kMinH), size.height(), qMax(double(Pane::kMinH), vp.height() - 48));
    const int i = spawnSeq_++;
    const double x = vp.left() + 24 + std::fmod(i * 30.0, 240.0);
    const double y = vp.top() + 24 + std::fmod(i * 30.0, 170.0);
    return QRectF(x, y, w, h);
}

// ---- background ------------------------------------------------------------

void Canvas::drawBackground(QPainter* p, const QRectF& rect) {
    const Theme::Colors c = Theme::currentColors();
    p->fillRect(rect, c.canvas);

    const double s = zoom();
    if (s < 0.35) return;  // the lattice degenerates into noise (and costs more than it shows)

    const double x0 = std::floor(rect.left() / kGrid) * kGrid;
    const double y0 = std::floor(rect.top() / kGrid) * kGrid;
    const int nx = int((rect.right() - x0) / kGrid) + 1;
    const int ny = int((rect.bottom() - y0) / kGrid) + 1;
    if (qint64(nx) * qint64(ny) > 60000) return;  // pathological zoom-out guard

    // Radial dots, drawn in SCENE coords so the lattice pans and zooms with the
    // panes (the CSS version painted them on the untransformed wrapper, so the
    // grid stayed put while the content moved — this is the fix, not a port bug).
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setPen(Qt::NoPen);
    p->setBrush(c.canvasGrid);
    const double r = 1.0 / qMax(0.35, qMin(s, 1.5));  // ≈1 device px at any zoom
    for (int ix = 0; ix <= nx; ++ix) {
        const double x = x0 + ix * kGrid;
        for (int iy = 0; iy <= ny; ++iy)
            p->drawEllipse(QPointF(x, y0 + iy * kGrid), r, r);
    }
    p->restore();
}

// ---- interaction -----------------------------------------------------------

void Canvas::mousePressEvent(QMouseEvent* e) {
    const QPoint pos = e->position().toPoint();
    // Middle-drag pans anywhere (even over a pane); left-drag pans only on empty
    // canvas, so a left-press on a pane still reaches the pane.
    const bool empty = itemAt(pos) == nullptr;
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && empty)) {
        panning_ = true;
        panLast_ = pos;
        restoreCursor_ = viewport()->cursor().shape();
        viewport()->setCursor(Qt::ClosedHandCursor);
        e->accept();
        return;
    }
    QGraphicsView::mousePressEvent(e);
}

void Canvas::mouseMoveEvent(QMouseEvent* e) {
    if (panning_) {
        const QPoint pos = e->position().toPoint();
        const QPoint d = pos - panLast_;
        panLast_ = pos;
        // Scroll in VIEWPORT pixels: the scrollbars are in view space, so this is
        // zoom-correct for free.
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - d.y());
        e->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(e);
}

void Canvas::mouseReleaseEvent(QMouseEvent* e) {
    if (panning_) {
        panning_ = false;
        viewport()->setCursor(restoreCursor_);
        emit viewChanged();
        e->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(e);
}

void Canvas::wheelEvent(QWheelEvent* e) {
    if (e->modifiers() & Qt::ControlModifier) {
        zoomBy(e->angleDelta().y() > 0 ? 1.1 : 0.9, e->position().toPoint());
        e->accept();
        return;
    }
    // A plain wheel belongs to whatever is under the cursor (a terminal's
    // scrollback, the editor); over empty canvas the view scrolls, which pans.
    QGraphicsView::wheelEvent(e);
}

void Canvas::contextMenuEvent(QContextMenuEvent* e) {
    if (itemAt(e->pos())) {
        QGraphicsView::contextMenuEvent(e);
        return;
    }
    emit contextMenuRequestedAt(e->globalPos(), mapToScene(e->pos()));
    e->accept();
}

void Canvas::resizeEvent(QResizeEvent* e) {
    QGraphicsView::resizeEvent(e);
    placeZoomControl();
}

void Canvas::refreshTheme() {
    resetCachedContent();
    viewport()->update();
    for (Pane* p : std::as_const(panes_)) p->update();
}

// ---- zoom control ----------------------------------------------------------

void Canvas::buildZoomControl() {
    zoomCtl_ = new QWidget(this);
    zoomCtl_->setObjectName("zoomCtl");
    auto* row = new QHBoxLayout(zoomCtl_);
    row->setContentsMargins(4, 4, 4, 4);
    row->setSpacing(4);

    auto mk = [&](const QString& text, const QString& tip) {
        auto* b = new QPushButton(text, zoomCtl_);
        b->setToolTip(tip);
        b->setFixedSize(26, 24);
        b->setCursor(Qt::PointingHandCursor);
        row->addWidget(b);
        return b;
    };

    QPushButton* out = mk(QStringLiteral("−"), tr("Zoom out"));
    zoomLabel_ = new QLabel(QStringLiteral("100%"), zoomCtl_);
    zoomLabel_->setAlignment(Qt::AlignCenter);
    zoomLabel_->setFixedWidth(46);
    zoomLabel_->setToolTip(tr("Current zoom (Ctrl+wheel zooms toward the cursor)"));
    row->addWidget(zoomLabel_);
    QPushButton* in = mk(QStringLiteral("+"), tr("Zoom in"));
    QPushButton* fit = mk(QStringLiteral("◎"), tr("Center — fit every window in view"));

    connect(out, &QPushButton::clicked, this, [this] { zoomBy(1.0 / 1.15); });
    connect(in, &QPushButton::clicked, this, [this] { zoomBy(1.15); });
    connect(fit, &QPushButton::clicked, this, &Canvas::centerAll);

    zoomCtl_->adjustSize();
    placeZoomControl();
}

void Canvas::placeZoomControl() {
    if (!zoomCtl_) return;
    zoomCtl_->adjustSize();
    zoomCtl_->move(width() - zoomCtl_->width() - 14, height() - zoomCtl_->height() - 14);
    zoomCtl_->raise();
}

void Canvas::syncZoomLabel() {
    if (zoomLabel_) zoomLabel_->setText(QString::number(int(std::lround(zoom() * 100))) + "%");
}

}  // namespace odv
