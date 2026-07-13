#include "TerminalWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "Pty.h"

namespace odv {
namespace {

constexpr int kWheelLines = 3;

}  // namespace

TerminalWidget::TerminalWidget(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);  // we always paint every pixel of the clip
    setAttribute(Qt::WA_InputMethodEnabled, false);
    setCursor(Qt::IBeamCursor);
    setContextMenuPolicy(Qt::DefaultContextMenu);

    font_ = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    // The system fixed font is whatever the platform theme says; keep it first but
    // give Qt a fallback chain so a bare/minimal box (or a CI container) still lands
    // on a real monospace instead of a proportional substitute.
    QStringList fams;
    if (!font_.family().isEmpty()) fams << font_.family();
    fams << QStringLiteral("JetBrains Mono") << QStringLiteral("Fira Code")
         << QStringLiteral("DejaVu Sans Mono") << QStringLiteral("monospace");
    font_.setFamilies(fams);
    font_.setStyleHint(QFont::Monospace);
    font_.setFixedPitch(true);
    if (font_.pointSizeF() <= 0) font_.setPointSizeF(10.5);

    applyFontMetrics();

    pty_ = new Pty(this);
    connect(pty_, &Pty::output, this, &TerminalWidget::onOutput);
    connect(pty_, &Pty::exited, this, [this](int code) {
        running_ = false;
        update();
        emit exited(code);
    });
}

TerminalWidget::~TerminalWidget() {
    if (pty_ && pty_->isRunning()) pty_->kill();
}

void TerminalWidget::applyFontMetrics() {
    const QFontMetricsF fm(font_);
    qreal adv = fm.horizontalAdvance(QLatin1Char('M'));
    if (adv <= 0) adv = fm.averageCharWidth();
    if (adv <= 0) adv = 8;

    // Snap the cell box to a whole number of DEVICE pixels. A fractional cell width
    // accumulates rounding error across 200 columns, so glyphs drift out of their
    // background rects; rounding up in device space and converting back keeps the
    // grid aligned at any devicePixelRatio (QPainter draws in logical units and
    // applies the DPR scale itself, so this is the only place it has to be handled).
    const qreal dpr = devicePixelRatioF() > 0 ? devicePixelRatioF() : 1.0;
    cellW_ = std::ceil(adv * dpr) / dpr;
    cellH_ = std::ceil(fm.height() * dpr) / dpr;
    baseline_ = fm.ascent();

    fontBold_ = font_;
    fontBold_.setBold(true);
    fontItalic_ = font_;
    fontItalic_.setItalic(true);
    fontBoldItalic_ = fontBold_;
    fontBoldItalic_.setItalic(true);
}

QSize TerminalWidget::sizeHint() const {
    return QSize(int(80 * cellW_), int(24 * cellH_));
}

// --- lifecycle --------------------------------------------------------------

void TerminalWidget::start(const QString& program, const QStringList& args, const QString& cwd) {
    if (pty_->isRunning()) return;

    QString prog = program;
    QStringList a = args;
    if (prog.isEmpty()) {
        prog = defaultShell();
        // Pty::start can't set argv[0], so we can't use the leading-dash convention;
        // -l is the portable way to ask bash/zsh for a login shell.
        if (a.isEmpty()) a << QStringLiteral("-l");
    }

    const QStringList env{
        QStringLiteral("TERM=xterm-256color"),
        QStringLiteral("COLORTERM=truecolor"),
    };

    syncSize();
    QString err;
    if (!pty_->start(prog, a, cwd, env, &err)) {
        running_ = false;
        vt_.appendHistory(tr("[failed to start %1: %2]").arg(prog, err));
        update();
        emit exited(-1);
        return;
    }
    running_ = true;
    update();
}

bool TerminalWidget::isRunning() const {
    return pty_ && pty_->isRunning();
}

void TerminalWidget::onOutput(const QByteArray& data) {
    const int sbBefore = int(vt_.scrollback().size());
    const int oldCurX = vt_.cursorX(), oldCurY = vt_.cursorY();

    vt_.feed(data);

    const int grew = int(vt_.scrollback().size()) - sbBefore;
    const VtParser::Dirty dirty = vt_.takeDirty();

    if (viewOffset_ > 0) {
        // The user is reading history. Hold their position instead of yanking them to
        // the bottom (which is what the JS renderer did): every line that just landed
        // in scrollback pushes the window one further back.
        viewOffset_ = std::min(viewOffset_ + grew, int(vt_.scrollback().size()));
        if (grew > 0) update();  // at the 5000-line cap the window slides anyway
    } else if (dirty.all) {
        update();
    } else {
        for (const int r : dirty.rows) update(rowRect(r));
        // The cursor's old and new rows need a repaint even if no cell under them
        // changed, or the block is left behind where it used to be.
        if (oldCurY != vt_.cursorY() || oldCurX != vt_.cursorX()) {
            update(rowRect(oldCurY));
            update(rowRect(vt_.cursorY()));
        }
    }
    lastCurX_ = vt_.cursorX();
    lastCurY_ = vt_.cursorY();

    QString t;
    if (vt_.takeTitle(&t)) emit titleChanged(t);
}

// --- geometry ---------------------------------------------------------------

void TerminalWidget::syncSize() {
    const int cols = std::max(2, int(width() / cellW_));
    const int rows = std::max(2, int(height() / cellH_));
    vt_.resize(cols, rows);
    // The whole resize story: the kernel is told the new window size and sends the
    // child SIGWINCH. No /proc walking, no stty -F, both of which the PHP app needed
    // because it never held the master fd.
    pty_->resize(cols, rows);
    viewOffset_ = std::min(viewOffset_, historyLines());
}

void TerminalWidget::resizeEvent(QResizeEvent* e) {
    applyFontMetrics();  // also picks up a devicePixelRatio change from a screen move
    syncSize();
    QWidget::resizeEvent(e);
}

QRect TerminalWidget::rowRect(int viewRow) const {
    return QRectF(0, viewRow * cellH_, width(), cellH_).toAlignedRect();
}

int TerminalWidget::historyLines() const {
    // The alt screen has no history — a TUI's full-screen repaints are not something
    // you scroll back through.
    return vt_.altScreen() ? 0 : int(vt_.scrollback().size());
}

int TerminalWidget::topLine() const {
    return historyLines() - viewOffset_;
}

const Cell* TerminalWidget::lineCells(int absLine, int* len) const {
    const int hist = historyLines();
    if (absLine < 0) return nullptr;
    if (absLine < hist) {
        const QVector<Cell>& row = vt_.scrollback().at(absLine);
        *len = int(row.size());  // may differ from cols(): history keeps the width it had
        return row.constData();
    }
    const int r = absLine - hist;
    if (r >= vt_.rows()) return nullptr;
    *len = vt_.cols();
    return vt_.screen().constData() + r * vt_.cols();
}

QPoint TerminalWidget::viewCellAt(const QPoint& px) const {
    const int c = std::clamp(int(px.x() / cellW_), 0, vt_.cols() - 1);
    const int r = std::clamp(int(px.y() / cellH_), 0, vt_.rows() - 1);
    return QPoint(c, r);
}

// --- painting ---------------------------------------------------------------

QColor TerminalWidget::fgOf(const Cell& c) const {
    const QColor f = c.fg ? QColor::fromRgb(QRgb(c.fg)) : defFg_;
    const QColor b = c.bg ? QColor::fromRgb(QRgb(c.bg)) : defBg_;
    return (c.attrs & AttrInverse) ? b : f;
}

QColor TerminalWidget::bgOf(const Cell& c) const {
    const QColor f = c.fg ? QColor::fromRgb(QRgb(c.fg)) : defFg_;
    const QColor b = c.bg ? QColor::fromRgb(QRgb(c.bg)) : defBg_;
    return (c.attrs & AttrInverse) ? f : b;
}

void TerminalWidget::paintEvent(QPaintEvent* e) {
    QPainter p(this);
    p.setRenderHint(QPainter::TextAntialiasing);
    const QRect clip = e->rect();
    p.fillRect(clip, defBg_);

    const int cols = vt_.cols(), rows = vt_.rows();
    const int r0 = std::max(0, int(clip.top() / cellH_));
    const int r1 = std::min(rows - 1, int(clip.bottom() / cellH_));
    const int top = topLine();

    for (int r = r0; r <= r1; ++r) {
        int len = 0;
        const Cell* row = lineCells(top + r, &len);
        if (!row) continue;
        const int abs = top + r;
        const qreal y = r * cellH_;

        // Run-length the row by style: one fillRect and one drawText per run instead
        // of per cell. Under a firehose (a build log, `yes`) the per-cell path is what
        // kills a terminal, and a typical row is only a handful of runs.
        int c = 0;
        while (c < cols) {
            const Cell base = (c < len) ? row[c] : Cell();
            const bool sel = inSelection(abs, c);
            int c2 = c + 1;
            while (c2 < cols) {
                const Cell n = (c2 < len) ? row[c2] : Cell();
                if (!n.sameStyle(base) || inSelection(abs, c2) != sel) break;
                ++c2;
            }

            const QRectF box(c * cellW_, y, (c2 - c) * cellW_, cellH_);
            const QColor bg = sel ? selBg_ : bgOf(base);
            if (bg != defBg_) p.fillRect(box, bg);

            QString run;
            run.reserve(c2 - c);
            for (int i = c; i < c2; ++i)
                run.append(i < len ? QString::fromUcs4(&row[i].ch, 1) : QStringLiteral(" "));

            if (!run.trimmed().isEmpty()) {
                const bool bold = base.attrs & AttrBold;
                const bool ital = base.attrs & AttrItalic;
                p.setFont(bold ? (ital ? fontBoldItalic_ : fontBold_)
                               : (ital ? fontItalic_ : font_));
                p.setPen(sel ? defBg_ : fgOf(base));
                if (base.attrs & AttrDim) p.setOpacity(0.6);
                p.drawText(QPointF(c * cellW_, y + baseline_), run);
                p.setOpacity(1.0);
                if (base.attrs & AttrUnderline) {
                    const qreal uy = y + cellH_ - 1.5;
                    p.drawLine(QPointF(c * cellW_, uy), QPointF(c2 * cellW_, uy));
                }
            }
            c = c2;
        }
    }

    // The cursor only exists on the live screen; scrolled back into history there is
    // nothing for it to sit on.
    if (vt_.cursorVisible() && viewOffset_ == 0) {
        const int cx = vt_.cursorX(), cy = vt_.cursorY();
        const QRectF box(cx * cellW_, cy * cellH_, cellW_, cellH_);
        if (box.intersects(QRectF(clip))) {
            int len = 0;
            const Cell* row = lineCells(top + cy, &len);
            const Cell under = (row && cx < len) ? row[cx] : Cell();
            if (hasFocus()) {
                p.fillRect(box, defFg_);
                p.setFont(font_);
                p.setPen(defBg_);
                p.drawText(QPointF(box.left(), cy * cellH_ + baseline_),
                           QString::fromUcs4(&under.ch, 1));
            } else {
                p.setPen(defFg_);
                p.drawRect(box.adjusted(0, 0, -1, -1));
            }
        }
    }
}

// --- input ------------------------------------------------------------------

QByteArray TerminalWidget::keyToBytes(QKeyEvent* e) const {
    const Qt::KeyboardModifiers m = e->modifiers();
    const int k = e->key();

    if (m & Qt::ControlModifier) {
        if (k >= Qt::Key_A && k <= Qt::Key_Z) return QByteArray(1, char(k - Qt::Key_A + 1));
        switch (k) {
            case Qt::Key_Space:
            case Qt::Key_2: return QByteArray(1, '\0');  // NUL
            case Qt::Key_BracketLeft: return "\x1b";
            case Qt::Key_Backslash: return "\x1c";
            case Qt::Key_BracketRight: return "\x1d";
            default: break;
        }
        // fall through: Ctrl+arrow etc. still want their normal sequence
    }

    QByteArray out;
    switch (k) {
        case Qt::Key_Return:
        case Qt::Key_Enter: return "\r";
        case Qt::Key_Backspace: return "\x7f";
        case Qt::Key_Tab: return "\t";
        case Qt::Key_Backtab: return "\x1b[Z";
        case Qt::Key_Escape: return "\x1b";
        case Qt::Key_Delete: return "\x1b[3~";
        case Qt::Key_PageUp: return "\x1b[5~";
        case Qt::Key_PageDown: return "\x1b[6~";
        case Qt::Key_Insert: return "\x1b[2~";
        // DECCKM: once an app (vim, less) turns on application cursor keys it expects
        // ESC O A. Sending ESC [ A there types a character instead of moving.
        case Qt::Key_Up: return vt_.applicationCursorKeys() ? "\x1bOA" : "\x1b[A";
        case Qt::Key_Down: return vt_.applicationCursorKeys() ? "\x1bOB" : "\x1b[B";
        case Qt::Key_Right: return vt_.applicationCursorKeys() ? "\x1bOC" : "\x1b[C";
        case Qt::Key_Left: return vt_.applicationCursorKeys() ? "\x1bOD" : "\x1b[D";
        case Qt::Key_Home: return vt_.applicationCursorKeys() ? "\x1bOH" : "\x1b[H";
        case Qt::Key_End: return vt_.applicationCursorKeys() ? "\x1bOF" : "\x1b[F";
        default: break;
    }
    if (m & Qt::ControlModifier) return out;  // unmapped Ctrl combo: send nothing

    const QString t = e->text();
    if (t.isEmpty()) return out;
    // Alt/Meta prefixes with ESC — that is how readline sees M-b / M-f.
    if (m & Qt::AltModifier) out.append('\x1b');
    out.append(t.toUtf8());
    return out;
}

void TerminalWidget::keyPressEvent(QKeyEvent* e) {
    const Qt::KeyboardModifiers m = e->modifiers();

    if ((m & Qt::ControlModifier) && (m & Qt::ShiftModifier)) {
        if (e->key() == Qt::Key_C) {
            copySelection();
            return;
        }
        if (e->key() == Qt::Key_V) {
            paste();
            return;
        }
    }
    // Plain Ctrl+C is NOT copy — it is SIGINT, and a terminal that steals it is
    // useless. That is why copy lives on Ctrl+Shift+C.

    if (e->key() == Qt::Key_Insert && (m & Qt::ShiftModifier)) {
        paste();
        return;
    }

    const QByteArray bytes = keyToBytes(e);
    if (bytes.isEmpty()) {
        QWidget::keyPressEvent(e);
        return;
    }
    if (viewOffset_ != 0) {  // typing snaps back to the prompt
        viewOffset_ = 0;
        update();
    }
    pty_->write(bytes);
    e->accept();
}

void TerminalWidget::sendText(const QString& text) {
    QString s = text;
    s.replace(QLatin1String("\r\n"), QLatin1String("\r"));
    s.replace(QLatin1Char('\n'), QLatin1Char('\r'));  // a tty's Enter is CR, not LF
    pty_->write(s.toUtf8());
}

// --- selection + clipboard --------------------------------------------------

// Clipboard here is just QClipboard. The PHP app needed a three-tier fallback (a
// hidden textarea for native paste, a WebKitGTK-specific flag on Linux because
// navigator.clipboard is blocked in a webview without it, and pbpaste/Get-Clipboard
// shellouts on macOS/Windows) purely to escape webview restrictions. None of that
// exists in a native widget.

bool TerminalWidget::hasSelection() const {
    return haveSel_ && selAnchor_ != selHead_;
}

void TerminalWidget::clearSelection() {
    if (!haveSel_) return;
    haveSel_ = false;
    update();
}

bool TerminalWidget::inSelection(int absLine, int col) const {
    if (!hasSelection()) return false;
    QPoint a = selAnchor_, b = selHead_;  // (col, absLine)
    if (a.y() > b.y() || (a.y() == b.y() && a.x() > b.x())) std::swap(a, b);
    if (absLine < a.y() || absLine > b.y()) return false;
    if (absLine == a.y() && col < a.x()) return false;
    if (absLine == b.y() && col >= b.x()) return false;  // head is exclusive
    return true;
}

QString TerminalWidget::selectionText() const {
    if (!hasSelection()) return QString();
    QPoint a = selAnchor_, b = selHead_;
    if (a.y() > b.y() || (a.y() == b.y() && a.x() > b.x())) std::swap(a, b);

    QStringList out;
    for (int line = a.y(); line <= b.y(); ++line) {
        int len = 0;
        const Cell* row = lineCells(line, &len);
        const int c0 = (line == a.y()) ? a.x() : 0;
        const int c1 = (line == b.y()) ? b.x() : vt_.cols();  // exclusive
        QString s;
        for (int c = c0; c < c1 && c < len; ++c)
            s.append(row ? QString::fromUcs4(&row[c].ch, 1) : QStringLiteral(" "));
        while (s.endsWith(QLatin1Char(' '))) s.chop(1);
        out.append(s);
    }
    return out.join(QLatin1Char('\n'));
}

void TerminalWidget::copySelection() {
    const QString s = selectionText();
    if (s.isEmpty()) return;
    QApplication::clipboard()->setText(s, QClipboard::Clipboard);
}

void TerminalWidget::paste() {
    const QString s = QApplication::clipboard()->text(QClipboard::Clipboard);
    if (!s.isEmpty()) sendText(s);
}

void TerminalWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    setFocus(Qt::MouseFocusReason);
    pressPx_ = e->pos();
    const QPoint c = viewCellAt(e->pos());
    selAnchor_ = QPoint(c.x(), topLine() + c.y());
    selHead_ = selAnchor_;
    selecting_ = true;
    if (haveSel_) {
        haveSel_ = false;
        update();
    }
}

void TerminalWidget::mouseMoveEvent(QMouseEvent* e) {
    if (!selecting_) return;
    // A few pixels of jitter while clicking is not a drag; without this every click
    // would leave a one-cell selection behind.
    if (!haveSel_ && (e->pos() - pressPx_).manhattanLength() < 4) return;
    haveSel_ = true;
    const QPoint c = viewCellAt(e->pos());
    selHead_ = QPoint(c.x(), topLine() + c.y());
    update();
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    selecting_ = false;
    if (!hasSelection()) return;
    // X11 PRIMARY: selecting text should make middle-click paste work, like every
    // other terminal. The CLIPBOARD selection is only touched by an explicit copy.
    const QString s = selectionText();
    if (!s.isEmpty() && QApplication::clipboard()->supportsSelection())
        QApplication::clipboard()->setText(s, QClipboard::Selection);
}

void TerminalWidget::wheelEvent(QWheelEvent* e) {
    if (vt_.altScreen()) {  // a TUI owns the screen; there is no history to scroll
        e->ignore();
        return;
    }
    const int dy = e->angleDelta().y();
    if (dy == 0) return;
    const int max = int(vt_.scrollback().size());
    const int next = std::clamp(viewOffset_ + (dy > 0 ? kWheelLines : -kWheelLines), 0, max);
    if (next != viewOffset_) {
        viewOffset_ = next;
        update();
    }
    e->accept();
}

void TerminalWidget::contextMenuEvent(QContextMenuEvent* e) {
    QMenu menu(this);
    QAction* copy = menu.addAction(tr("Copy"));
    copy->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    copy->setEnabled(hasSelection());
    QAction* pasteAct = menu.addAction(tr("Paste"));
    pasteAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+V")));
    pasteAct->setEnabled(!QApplication::clipboard()->text().isEmpty());

    QAction* chosen = menu.exec(e->globalPos());
    if (chosen == copy) copySelection();
    else if (chosen == pasteAct) paste();
}

void TerminalWidget::focusInEvent(QFocusEvent* e) {
    update(rowRect(vt_.cursorY()));
    QWidget::focusInEvent(e);
}

void TerminalWidget::focusOutEvent(QFocusEvent* e) {
    update(rowRect(vt_.cursorY()));
    QWidget::focusOutEvent(e);
}

// --- session restore --------------------------------------------------------

QString TerminalWidget::snapshot() const {
    return vt_.dumpText();
}

void TerminalWidget::replay(const QString& text) {
    if (text.isEmpty()) return;
    // Straight into scrollback, not through the screen: restored output is history,
    // and the live shell must start on a clean screen below it.
    vt_.appendHistory(text);
    viewOffset_ = 0;
    update();
}

}  // namespace odv
