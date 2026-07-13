#pragma once
#include <QGraphicsWidget>
#include <QPointF>
#include <QRectF>
#include <QString>

class QGraphicsProxyWidget;
class QWidget;

namespace odv {

// A floating window on the canvas.
//
// WHY QGraphicsWidget + a proxy for the CONTENT only (rather than putting the
// whole frame, chrome included, inside one QGraphicsProxyWidget):
//   - The chrome is painted by the item, so it stays crisp at any zoom and its
//     hit targets (title bar, ⤢, ✕, resize grip) are tested in scene coords.
//     Drag maths therefore needs no zoom compensation at all — scene deltas are
//     already world deltas, which is exactly the "/ zoom" fudge wireFree() in
//     app.js had to do by hand.
//   - A proxied chrome would route those presses through Qt's widget event
//     mapping, and the pane would fight the view's own pan/rubber-band handling.
// The content is a real QWidget (TerminalWidget, EditorPane, …), so it must be
// proxied; that is the one place a proxy is unavoidable.
class Pane : public QGraphicsWidget {
    Q_OBJECT

public:
    static constexpr qreal kHeadH = 26.0;
    static constexpr qreal kGrip = 16.0;
    static constexpr qreal kMinW = 240.0;  // same floor as the PHP canvas
    static constexpr qreal kMinH = 130.0;

    // Takes ownership of `content` (via the proxy).
    Pane(const QString& id, const QString& title, QWidget* content,
         QGraphicsItem* parent = nullptr);

    QString id() const { return id_; }
    QWidget* content() const { return content_; }
    QString title() const { return title_; }
    void setTitle(const QString& t);

    QRectF geometryF() const { return geometry(); }
    void setGeometryF(const QRectF& r);

    // The canvas owns the focus/maximise state (only one pane may be maximised),
    // so it tells the pane which glyph to draw.
    void setMaximised(bool on);
    bool isMaximised() const { return maximised_; }

signals:
    void closeRequested();       // emitted ONLY after the user confirms
    void raised();               // clicked anywhere — the canvas re-stacks
    void focusToggleRequested();
    void geometryEdited();       // a move/resize finished

protected:
    void paint(QPainter* p, const QStyleOptionGraphicsItem* o, QWidget* w) override;
    void resizeEvent(QGraphicsSceneResizeEvent* e) override;
    void mousePressEvent(QGraphicsSceneMouseEvent* e) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent* e) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* e) override;
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* e) override;
    void hoverMoveEvent(QGraphicsSceneHoverEvent* e) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* e) override;

private:
    enum class Drag { None, Move, Resize };

    QRectF headRect() const;
    QRectF closeRect() const;
    QRectF zoomRect() const;
    QRectF gripRect() const;
    void layoutContent();
    void askThenClose();

    QString id_;
    QString title_;
    QWidget* content_ = nullptr;
    QGraphicsProxyWidget* proxy_ = nullptr;

    Drag drag_ = Drag::None;
    QPointF dragOrigin_;   // scene pos at press
    QRectF dragGeom_;      // geometry at press
    int hover_ = 0;        // 0 none, 1 close, 2 zoom, 3 grip
    bool maximised_ = false;
    bool closing_ = false;  // a confirm dialog is already up
};

}  // namespace odv
