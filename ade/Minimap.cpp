#include "Minimap.h"

#include <QMouseEvent>
#include <QPainter>

#include "Canvas.h"
#include "Pane.h"
#include "Theme.h"

namespace odv {
namespace {
constexpr int kW = 168;
constexpr int kH = 110;
constexpr int kPad = 12;   // inset from the canvas edge
constexpr double kMargin = 200.0;  // world units of breathing room around the content
}  // namespace

Minimap::Minimap(Canvas* canvas, QWidget* parent)
    : QWidget(parent ? parent : canvas), canvas_(canvas) {
    setFixedSize(kW, kH);
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr("Where you are. Click or drag to go somewhere."));
    setAttribute(Qt::WA_TransparentForMouseEvents, false);

    // Repaint whenever the view moves or a pane does. viewChanged covers pan, zoom
    // and fit; a pane being dragged emits it too, because the canvas owns the scene.
    connect(canvas_, &Canvas::viewChanged, this, [this] { update(); });
    reposition();
    raise();
}

void Minimap::reposition() {
    if (!parentWidget()) return;
    move(kPad, parentWidget()->height() - kH - kPad);
    raise();
}

QRectF Minimap::worldRect() const {
    QRectF r;
    for (Pane* p : canvas_->panes()) {
        if (!p) continue;
        const QRectF g(p->pos(), p->size());
        r = r.isNull() ? g : r.united(g);
    }
    // Always include where you are LOOKING, or panning into empty space would walk
    // the viewport box off the edge of its own map.
    const QRectF vp = canvas_->viewportWorldRect();
    r = r.isNull() ? vp : r.united(vp);

    r = r.adjusted(-kMargin, -kMargin, kMargin, kMargin);
    // Never let it collapse: a single small pane would otherwise be magnified to
    // fill the map, and every jitter would look like an earthquake.
    if (r.width() < 800) r.adjust(-(800 - r.width()) / 2, 0, (800 - r.width()) / 2, 0);
    if (r.height() < 600) r.adjust(0, -(600 - r.height()) / 2, 0, (600 - r.height()) / 2);
    return r;
}

QPointF Minimap::mapToWorld(const QPointF& widgetPos) const {
    const QRectF w = worldRect();
    const double sx = w.width() / qMax(1, width());
    const double sy = w.height() / qMax(1, height());
    const double s = qMax(sx, sy);  // uniform: a squashed map lies about direction
    // The content is centred in the widget, so undo that offset first.
    const double ox = (width() - w.width() / s) / 2.0;
    const double oy = (height() - w.height() / s) / 2.0;
    return QPointF(w.left() + (widgetPos.x() - ox) * s, w.top() + (widgetPos.y() - oy) * s);
}

void Minimap::paintEvent(QPaintEvent*) {
    const Theme::Colors c = Theme::currentColors();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    QColor bg = c.bg2;
    bg.setAlpha(225);  // legible over whatever is behind, without hiding it entirely
    p.setPen(QPen(c.border, 1));
    p.setBrush(bg);
    p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 6, 6);

    const QRectF w = worldRect();
    const double s = qMax(w.width() / qMax(1, width()), w.height() / qMax(1, height()));
    if (s <= 0) return;
    const double ox = (width() - w.width() / s) / 2.0;
    const double oy = (height() - w.height() / s) / 2.0;
    const auto toMap = [&](const QRectF& r) {
        return QRectF(ox + (r.left() - w.left()) / s, oy + (r.top() - w.top()) / s,
                      r.width() / s, r.height() / s);
    };

    // The panes.
    for (Pane* pane : canvas_->panes()) {
        if (!pane) continue;
        const QRectF r = toMap(QRectF(pane->pos(), pane->size()));
        p.setPen(Qt::NoPen);
        p.setBrush(c.elev);
        p.drawRect(r);
        p.setPen(QPen(c.faint, 0.8));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r);
    }

    // Where you are looking. Drawn last, so it is never hidden behind a pane.
    p.setPen(QPen(c.accent, 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawRect(toMap(canvas_->viewportWorldRect()));
}

void Minimap::jumpTo(const QPointF& widgetPos) {
    const QPointF centre = mapToWorld(widgetPos);
    const QRectF vp = canvas_->viewportWorldRect();
    // Centre the viewport on the click, rather than putting its corner there — you
    // are pointing at what you want to LOOK at.
    canvas_->setView(centre - QPointF(vp.width() / 2, vp.height() / 2), canvas_->zoom());
    update();
}

void Minimap::mousePressEvent(QMouseEvent* e) {
    jumpTo(e->position());
    e->accept();
}

void Minimap::mouseMoveEvent(QMouseEvent* e) {
    if (e->buttons() & Qt::LeftButton) jumpTo(e->position());
    e->accept();
}

}  // namespace odv
