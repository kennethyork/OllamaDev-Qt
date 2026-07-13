#pragma once

#include <QByteArray>
#include <QChar>
#include <QString>
#include <QVector>
#include <QtGlobal>

namespace odv {

// Attribute bits packed into Cell::attrs.
enum CellAttr : quint8 {
    AttrBold = 1u << 0,
    AttrDim = 1u << 1,
    AttrItalic = 1u << 2,
    AttrUnderline = 1u << 3,
    AttrInverse = 1u << 4,
};

// One character cell.
//
// fg/bg are 0 for "terminal default" (the widget substitutes its theme colours);
// a set colour is 0xFF000000 | 0xRRGGBB, so a non-zero alpha is what distinguishes
// "explicitly black" from "unset". We pack into quint32 rather than hold a QColor
// because a screen is rows*cols of these and an 80x24 grid churns through 1920 of
// them on every repaint — QColor is 16 bytes and non-trivial to copy.
//
// CONSTRAINT: one QChar is one UTF-16 code unit, so a cell cannot hold an astral
// codepoint (emoji, U+10000+). Those are stored as U+FFFD rather than split across
// two cells as surrogate halves, which is what the JS renderer did — splitting
// corrupts textAt()/snapshot() output and paints two tofu boxes anyway.
struct Cell {
    // char32_t, not QChar: a QChar is one UTF-16 unit and cannot hold an astral
    // codepoint, so every emoji became U+FFFD. That is not academic — this app's
    // own CLI prints "💭 thought for Ns" (U+1F4AD), and the terminal would have
    // rendered its own output as tofu. Splitting astral chars across two cells as
    // surrogate halves (what the JS version did) is worse: it corrupts selection
    // and snapshot text AND still paints two boxes.
    char32_t ch = U' ';
    quint32 fg = 0;
    quint32 bg = 0;
    quint8 attrs = 0;

    bool sameStyle(const Cell& o) const { return fg == o.fg && bg == o.bg && attrs == o.attrs; }
    bool operator==(const Cell& o) const { return ch == o.ch && sameStyle(o); }
};

// A cell-grid VT/xterm emulator: bytes in, a styled grid out.
//
// Deliberately not a QObject and not tied to a widget — TerminalWidget owns one and
// paints it, but the CLI can also drive one headlessly to render a captured session.
//
// feed() is re-entrant across arbitrary chunk boundaries: the escape state machine
// and the UTF-8 decoder both keep their partial state in members, because the pty
// hands us whatever the kernel had ready and WILL split a CSI sequence or a
// multi-byte codepoint down the middle.
class VtParser {
public:
    // Lines of primary-screen history retained. The alt screen never feeds this.
    static constexpr int kMaxScrollback = 5000;

    VtParser();

    void resize(int cols, int rows);
    void feed(const QByteArray& bytes);

    const QVector<Cell>& screen() const;  // active screen, rows*cols, row-major
    const QVector<QVector<Cell>>& scrollback() const { return sb_; }

    int cols() const { return cols_; }
    int rows() const { return rows_; }
    int cursorX() const;  // clamped into [0, cols-1] even when a wrap is pending
    int cursorY() const { return cy_; }
    bool cursorVisible() const { return cursorVis_; }
    bool altScreen() const { return alt_; }

    void clear();  // wipes screen AND scrollback, homes the cursor, resets SGR

    // Plain text of the rectangle's *linear* span (as a selection reads, not a
    // block): from (x0,y0) to (x1,y1) in screen coordinates, trailing blanks
    // trimmed per line. Ends may be given in either order.
    QString textAt(int x0, int y0, int x1, int y1) const;

    // --- beyond the required surface ---------------------------------------

    // DECCKM (CSI ?1 h/l). vim/less switch this on and then expect ESC O A for the
    // arrow keys instead of ESC [ A; sending the wrong one moves the cursor in
    // insert mode instead of scrolling.
    bool applicationCursorKeys() const { return appCursor_; }

    // OSC 0/2 window title, if one arrived since the last call. Every other OSC is
    // parsed and dropped.
    bool takeTitle(QString* out);

    // Append plain text straight into scrollback as read-only history, without
    // running it through the screen. Used by TerminalWidget::replay() to restore a
    // previous session's output above a fresh shell.
    void appendHistory(const QString& text);

    // scrollback + screen as plain text, trailing blank lines dropped.
    QString dumpText() const;

    // Rows touched since the last call. `all` means a scroll/resize/alt-switch moved
    // every row and a partial repaint would tear.
    struct Dirty {
        bool all = false;
        QVector<int> rows;
    };
    Dirty takeDirty();

private:
    enum class St {
        Ground,
        Esc,
        EscIntermediate,  // ESC ( ) * + : swallow the charset designator byte
        Csi,
        Str,     // OSC / DCS / APC / PM / SOS body, terminated by BEL or ST
        StrEsc,  // saw ESC inside a string; a '\' makes it ST
    };

    QVector<Cell>& active() { return alt_ ? altScr_ : pri_; }
    const QVector<Cell>& activeConst() const { return alt_ ? altScr_ : pri_; }

    Cell* cellAt(int x, int y);
    void markRow(int r);
    void markAll();

    void blankRow(int r);
    void blankRange(int r, int x0, int x1);
    void clampCursor();

    void putChar(uint cp);
    void lineFeed();
    void reverseIndex();
    void tab();

    void scrollUp(int n);
    void scrollDown(int n);
    void insertLines(int n);
    void deleteLines(int n);
    void insertChars(int n);
    void deleteChars(int n);
    void eraseChars(int n);
    void eraseLine(int mode);
    void eraseDisplay(int mode);

    void csiDispatch(char final);
    void sgr(const QVector<int>& ps);
    void mode(const QVector<int>& ps, bool set);
    void enterAlt();
    void leaveAlt();
    void saveCursor();
    void restoreCursor();
    void reset();

    void pushScrollback(int row);
    bool utf8Push(quint8 b, uint* out);

    int cols_ = 80;
    int rows_ = 24;

    QVector<Cell> pri_;     // primary screen, rows_*cols_
    QVector<Cell> altScr_;  // alternate screen, same shape
    QVector<QVector<Cell>> sb_;

    int cx_ = 0;
    int cy_ = 0;
    int top_ = 0;               // DECSTBM scroll region, inclusive
    int bot_ = 23;              //
    Cell pen_;                  // current SGR state
    bool alt_ = false;
    bool cursorVis_ = true;
    bool appCursor_ = false;

    struct Saved {
        int x = 0;
        int y = 0;
        Cell pen;
        bool valid = false;
    };
    Saved savedPri_;  // DECSC/DECRC are per-screen, and ?1049 uses the primary slot
    Saved savedAlt_;

    St state_ = St::Ground;
    QByteArray csiParams_;  // 0x30-0x3F: digits, ';', ':', and the private '?' marker
    QByteArray csiInter_;   // 0x20-0x2F intermediates; non-empty => we ignore the seq
    QByteArray strBuf_;
    char strKind_ = 0;  // ']' for OSC, 'P' for DCS, ...
    QString title_;
    bool titleDirty_ = false;

    uint uniAcc_ = 0;  // partial UTF-8 codepoint carried across feed() calls
    int uniNeed_ = 0;
    int uniGot_ = 0;

    bool dirtyAll_ = true;
    QVector<bool> dirtyRow_;
};

}  // namespace odv
