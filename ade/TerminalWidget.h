#pragma once

#include <QColor>
#include <QFont>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "VtParser.h"

namespace odv {

class Pty;

// A terminal: owns a Pty, feeds its bytes to a VtParser, paints the grid.
//
// The PHP original could not do this. It ran the shell under `script -qfc`, polled
// two bridge files from JS every 12-150 ms, and rendered into DOM nodes; resizing
// meant walking /proc/<pid>/fd/0 to find the pts and shelling out to `stty`. Here the
// pty master fd is ours, so output arrives on the event loop the moment the kernel
// has it, and a resize is a single ioctl inside Pty.
class TerminalWidget : public QWidget {
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget* parent = nullptr);
    ~TerminalWidget() override;

    // Starts a login shell if `program` is empty.
    void start(const QString& program, const QStringList& args, const QString& cwd);

    void sendText(const QString& text);
    void copySelection();
    void paste();

    QString snapshot() const;          // plain-text scrollback, for session restore
    void replay(const QString& text);  // paint restored scrollback as read-only history
    bool isRunning() const;

    QSize sizeHint() const override;

signals:
    void exited(int code);
    void titleChanged(const QString& t);

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void focusInEvent(QFocusEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;

private:
    void onOutput(const QByteArray& data);
    void applyFontMetrics();
    void syncSize();  // widget pixels -> cols/rows -> VtParser + Pty
    QRect rowRect(int viewRow) const;

    // A viewport row (0..rows-1) maps to an absolute line: scrollback lines come
    // first, then the live screen. Scrolling back just slides that window.
    int historyLines() const;
    int topLine() const;
    const Cell* lineCells(int absLine, int* len) const;  // nullptr => blank row

    QPoint viewCellAt(const QPoint& px) const;  // pixel -> (col, row), clamped
    QString selectionText() const;
    bool hasSelection() const;
    bool inSelection(int absLine, int col) const;
    void clearSelection();

    QByteArray keyToBytes(QKeyEvent* e) const;
    QColor fgOf(const Cell& c) const;
    QColor bgOf(const Cell& c) const;

    Pty* pty_ = nullptr;
    VtParser vt_;

    QFont font_;
    QFont fontBold_;
    QFont fontItalic_;
    QFont fontBoldItalic_;
    qreal cellW_ = 8;
    qreal cellH_ = 16;
    qreal baseline_ = 12;

    QColor defFg_{0xC9, 0xD1, 0xD9};  // GitHub-dark, matching the PHP renderer
    QColor defBg_{0x0D, 0x11, 0x17};
    QColor selBg_{0x2F, 0x6F, 0xEB};

    int viewOffset_ = 0;  // lines scrolled back from the bottom; 0 = live

    // Selection anchors are ABSOLUTE line numbers, not viewport rows, so a drag that
    // scrolls (or output arriving mid-drag) doesn't smear the selection onto
    // whatever text slid under the mouse.
    bool selecting_ = false;
    bool haveSel_ = false;
    QPoint selAnchor_;  // (col, absLine)
    QPoint selHead_;
    QPoint pressPx_;

    int lastCurX_ = 0;
    int lastCurY_ = 0;
    bool running_ = false;
};

}  // namespace odv
