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
#include <QWidget>

namespace odv {

class Canvas;

// The overview map, bottom-left of the canvas.
//
// An OVERLAY on the canvas, deliberately not a pane. A minimap that lived inside a
// draggable window you could pan away from — losing the thing that tells you where
// you are — would be a joke at the user's expense.
//
// It exists because an infinite canvas has one failure mode: you pan somewhere,
// lose your panes off-screen, and have no idea which direction they are in. There
// is a fit-all button, but that is a sledgehammer — it moves you, rather than
// telling you where you already are.
class Minimap : public QWidget {
    Q_OBJECT

public:
    explicit Minimap(Canvas* canvas, QWidget* parent = nullptr);

    // Re-place ourselves in the canvas's bottom-left. Called on resize.
    void reposition();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;

private:
    // The world rect the map covers: every pane, plus the current viewport, plus a
    // margin. It is the union rather than a fixed area because the interesting part
    // of an infinite canvas is only ever the part you have actually used.
    QRectF worldRect() const;
    QPointF mapToWorld(const QPointF& widgetPos) const;

    void jumpTo(const QPointF& widgetPos);

    Canvas* canvas_;
};

}  // namespace odv
