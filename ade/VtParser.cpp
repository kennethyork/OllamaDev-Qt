#include "VtParser.h"

#include <QStringList>

#include <algorithm>

namespace odv {
namespace {

// The 16 ANSI colours, GitHub-dark tones — the same table the PHP/JS renderer used,
// so a session looks identical across the two apps.
constexpr quint32 kPal16[16] = {
    0xFF484F58, 0xFFF85149, 0xFF3FB950, 0xFFD29922, 0xFF58A6FF, 0xFFBC8CFF, 0xFF39C5CF, 0xFFB1BAC4,
    0xFF6E7681, 0xFFFF7B72, 0xFF56D364, 0xFFE3B341, 0xFF79C0FF, 0xFFD2A8FF, 0xFF56D4DD, 0xFFF0F6FC,
};

quint32 rgb(int r, int g, int b) {
    return 0xFF000000u | (quint32(r & 0xFF) << 16) | (quint32(g & 0xFF) << 8) | quint32(b & 0xFF);
}

// xterm-256: 0-15 the palette above, 16-231 a 6x6x6 cube, 232-255 a 24-step ramp.
quint32 xterm256(int n) {
    if (n < 0 || n > 255) return 0;
    if (n < 16) return kPal16[n];
    if (n < 232) {
        n -= 16;
        const int r = n / 36, g = (n % 36) / 6, b = n % 6;
        const auto lv = [](int v) { return v == 0 ? 0 : 55 + v * 40; };
        return rgb(lv(r), lv(g), lv(b));
    }
    const int v = 8 + (n - 232) * 10;
    return rgb(v, v, v);
}

QString rstrip(const QString& s) {
    int e = s.size();
    while (e > 0 && (s[e - 1] == QLatin1Char(' ') || s[e - 1] == QLatin1Char('\t'))) --e;
    return s.left(e);
}

}  // namespace

VtParser::VtParser() {
    resize(cols_, rows_);
}

// --- geometry ---------------------------------------------------------------

void VtParser::resize(int cols, int rows) {
    cols = std::max(2, cols);
    rows = std::max(2, rows);
    if (cols == cols_ && rows == rows_ && !pri_.isEmpty()) return;

    const int oc = cols_, orows = rows_;
    const QVector<Cell> oldPri = pri_, oldAlt = altScr_;
    const bool had = !oldPri.isEmpty();

    // Anchor to the BOTTOM when shrinking: the prompt lives at the cursor, and
    // dropping the rows below it (what a naive top-left copy does) would throw away
    // the only part of the screen the user is looking at. Rows pushed off the top go
    // to scrollback, exactly as if the shell had scrolled them.
    int drop = 0;
    if (had && rows < orows) drop = std::min(orows - rows, cy_);

    QVector<Cell> np(cols * rows), na(cols * rows);
    if (had) {
        for (int r = 0; r < rows; ++r) {
            const int src = r + drop;
            if (src >= orows) break;
            for (int c = 0; c < cols && c < oc; ++c) {
                np[r * cols + c] = oldPri[src * oc + c];
                na[r * cols + c] = oldAlt[src * oc + c];
            }
        }
        for (int r = 0; r < drop; ++r) {
            QVector<Cell> line(oc);
            for (int c = 0; c < oc; ++c) line[c] = oldPri[r * oc + c];
            sb_.append(line);
        }
        while (sb_.size() > kMaxScrollback) sb_.removeFirst();
        cy_ -= drop;
    }

    cols_ = cols;
    rows_ = rows;
    pri_ = np;
    altScr_ = na;
    dirtyRow_ = QVector<bool>(rows, true);

    // A resize invalidates the scroll region: the app is about to be told about the
    // new size (SIGWINCH) and will re-issue DECSTBM if it cares.
    top_ = 0;
    bot_ = rows_ - 1;
    clampCursor();
    markAll();
}

const QVector<Cell>& VtParser::screen() const {
    return activeConst();
}

int VtParser::cursorX() const {
    return std::clamp(cx_, 0, cols_ - 1);
}

Cell* VtParser::cellAt(int x, int y) {
    if (x < 0 || y < 0 || x >= cols_ || y >= rows_) return nullptr;
    return &active()[y * cols_ + x];
}

void VtParser::markRow(int r) {
    if (r >= 0 && r < dirtyRow_.size()) dirtyRow_[r] = true;
}

void VtParser::markAll() {
    dirtyAll_ = true;
}

VtParser::Dirty VtParser::takeDirty() {
    Dirty d;
    d.all = dirtyAll_;
    if (!d.all) {
        for (int r = 0; r < dirtyRow_.size(); ++r)
            if (dirtyRow_[r]) d.rows.append(r);
    }
    dirtyAll_ = false;
    dirtyRow_.fill(false);
    return d;
}

void VtParser::blankRow(int r) {
    blankRange(r, 0, cols_ - 1);
}

void VtParser::blankRange(int r, int x0, int x1) {
    if (r < 0 || r >= rows_) return;
    QVector<Cell>& scr = active();
    Cell blank;
    // Erasures keep the current background (BCE) so `clear` under a themed prompt
    // doesn't leave the old colour behind, but never keep the glyph attributes.
    blank.bg = pen_.bg;
    for (int c = std::max(0, x0); c <= std::min(cols_ - 1, x1); ++c) scr[r * cols_ + c] = blank;
    markRow(r);
}

void VtParser::clampCursor() {
    cx_ = std::clamp(cx_, 0, cols_ - 1);
    cy_ = std::clamp(cy_, 0, rows_ - 1);
}

void VtParser::clear() {
    // Reset the pen FIRST: erasing keeps the current background (BCE), so clearing
    // while a themed prompt has left a background set would fill the screen with it.
    pen_ = Cell();
    for (int r = 0; r < rows_; ++r) blankRow(r);
    sb_.clear();
    cx_ = cy_ = 0;
    markAll();
}

void VtParser::reset() {
    pen_ = Cell();
    for (int r = 0; r < rows_; ++r) blankRow(r);
    cx_ = cy_ = 0;
    top_ = 0;
    bot_ = rows_ - 1;
    cursorVis_ = true;
    appCursor_ = false;
    markAll();
}

// --- scrolling --------------------------------------------------------------

void VtParser::pushScrollback(int row) {
    QVector<Cell> line(cols_);
    const QVector<Cell>& scr = activeConst();
    for (int c = 0; c < cols_; ++c) line[c] = scr[row * cols_ + c];
    sb_.append(line);
    if (sb_.size() > kMaxScrollback) sb_.removeFirst();
}

void VtParser::scrollUp(int n) {
    QVector<Cell>& scr = active();
    for (int i = 0; i < n; ++i) {
        // Only lines leaving the TOP of the screen become history. A scroll inside a
        // DECSTBM region (a TUI's status-bar layout) is not history, and the alt
        // screen never is — that is the whole point of the alt screen.
        if (!alt_ && top_ == 0) pushScrollback(0);
        for (int r = top_; r < bot_; ++r)
            std::copy(scr.begin() + (r + 1) * cols_, scr.begin() + (r + 2) * cols_,
                      scr.begin() + r * cols_);
        blankRow(bot_);
    }
    markAll();
}

void VtParser::scrollDown(int n) {
    QVector<Cell>& scr = active();
    for (int i = 0; i < n; ++i) {
        for (int r = bot_; r > top_; --r)
            std::copy(scr.begin() + (r - 1) * cols_, scr.begin() + r * cols_,
                      scr.begin() + r * cols_);
        blankRow(top_);
    }
    markAll();
}

void VtParser::insertLines(int n) {
    if (cy_ < top_ || cy_ > bot_) return;
    QVector<Cell>& scr = active();
    n = std::min(n, bot_ - cy_ + 1);
    for (int i = 0; i < n; ++i) {
        for (int r = bot_; r > cy_; --r)
            std::copy(scr.begin() + (r - 1) * cols_, scr.begin() + r * cols_,
                      scr.begin() + r * cols_);
        blankRow(cy_);
    }
    markAll();
}

void VtParser::deleteLines(int n) {
    if (cy_ < top_ || cy_ > bot_) return;
    QVector<Cell>& scr = active();
    n = std::min(n, bot_ - cy_ + 1);
    for (int i = 0; i < n; ++i) {
        for (int r = cy_; r < bot_; ++r)
            std::copy(scr.begin() + (r + 1) * cols_, scr.begin() + (r + 2) * cols_,
                      scr.begin() + r * cols_);
        blankRow(bot_);
    }
    markAll();
}

void VtParser::insertChars(int n) {
    QVector<Cell>& scr = active();
    const int x = cursorX();
    n = std::min(n, cols_ - x);
    Cell blank;
    blank.bg = pen_.bg;
    for (int c = cols_ - 1; c >= x + n; --c) scr[cy_ * cols_ + c] = scr[cy_ * cols_ + c - n];
    for (int c = x; c < x + n; ++c) scr[cy_ * cols_ + c] = blank;
    markRow(cy_);
}

void VtParser::deleteChars(int n) {
    QVector<Cell>& scr = active();
    const int x = cursorX();
    n = std::min(n, cols_ - x);
    Cell blank;
    blank.bg = pen_.bg;
    for (int c = x; c < cols_ - n; ++c) scr[cy_ * cols_ + c] = scr[cy_ * cols_ + c + n];
    for (int c = cols_ - n; c < cols_; ++c) scr[cy_ * cols_ + c] = blank;
    markRow(cy_);
}

void VtParser::eraseChars(int n) {
    const int x = cursorX();
    blankRange(cy_, x, std::min(cols_ - 1, x + n - 1));
}

void VtParser::eraseLine(int mode) {
    const int x = cursorX();
    if (mode == 1) blankRange(cy_, 0, x);
    else if (mode == 2) blankRow(cy_);
    else blankRange(cy_, x, cols_ - 1);
}

void VtParser::eraseDisplay(int mode) {
    if (mode == 2 || mode == 3) {
        for (int r = 0; r < rows_; ++r) blankRow(r);
        // ED 3 is xterm's "drop the saved lines" — what `clear` sends so the old
        // screen can't be scrolled back to.
        if (mode == 3) sb_.clear();
        // NB: ED does NOT home the cursor. `clear` gets away with it because it
        // sends CUP first; a TUI that repaints in place relies on staying put.
        markAll();
        return;
    }
    if (mode == 1) {
        blankRange(cy_, 0, cursorX());
        for (int r = 0; r < cy_; ++r) blankRow(r);
    } else {
        blankRange(cy_, cursorX(), cols_ - 1);
        for (int r = cy_ + 1; r < rows_; ++r) blankRow(r);
    }
    markAll();
}

// --- printing ---------------------------------------------------------------

void VtParser::lineFeed() {
    if (cy_ == bot_) scrollUp(1);
    else if (cy_ < rows_ - 1) ++cy_;
}

void VtParser::reverseIndex() {
    if (cy_ == top_) scrollDown(1);
    else if (cy_ > 0) --cy_;
}

void VtParser::tab() {
    // Fixed 8-column stops. We move the cursor rather than writing spaces (which is
    // what the JS did): overwriting with blanks would erase cells a redraw had just
    // put there, and a real VT only moves.
    int x = cursorX();
    x = std::min(cols_ - 1, (x / 8 + 1) * 8);
    cx_ = x;
}

void VtParser::putChar(uint cp) {
    // Deferred wrap: after a glyph lands in the last column the cursor parks at
    // cols_, one past the end. It only wraps when the NEXT glyph arrives, so a line
    // that exactly fills the width doesn't drag a phantom blank line behind it.
    if (cx_ >= cols_) {
        cx_ = 0;
        lineFeed();
    }
    Cell* c = cellAt(cx_, cy_);
    if (!c) return;
    c->ch = (cp > 0xFFFF) ? QChar(QChar::ReplacementCharacter) : QChar(ushort(cp));
    c->fg = pen_.fg;
    c->bg = pen_.bg;
    c->attrs = pen_.attrs;
    markRow(cy_);
    ++cx_;
}

// --- SGR / modes ------------------------------------------------------------

void VtParser::sgr(const QVector<int>& ps) {
    if (ps.isEmpty()) {
        pen_ = Cell();
        return;
    }
    for (int i = 0; i < ps.size(); ++i) {
        const int c = ps[i];
        if (c == 0) pen_ = Cell();
        else if (c == 1) pen_.attrs |= AttrBold;
        else if (c == 2) pen_.attrs |= AttrDim;
        else if (c == 3) pen_.attrs |= AttrItalic;
        else if (c == 4) pen_.attrs |= AttrUnderline;
        else if (c == 7) pen_.attrs |= AttrInverse;
        else if (c == 22) pen_.attrs &= ~(AttrBold | AttrDim);
        else if (c == 23) pen_.attrs &= ~quint8(AttrItalic);
        else if (c == 24) pen_.attrs &= ~quint8(AttrUnderline);
        else if (c == 27) pen_.attrs &= ~quint8(AttrInverse);
        else if (c == 39) pen_.fg = 0;
        else if (c == 49) pen_.bg = 0;
        else if (c == 38 || c == 48) {
            quint32& t = (c == 38) ? pen_.fg : pen_.bg;
            if (i + 1 < ps.size() && ps[i + 1] == 5 && i + 2 < ps.size()) {
                t = xterm256(ps[i + 2]);
                i += 2;
            } else if (i + 1 < ps.size() && ps[i + 1] == 2 && i + 4 < ps.size()) {
                t = rgb(ps[i + 2], ps[i + 3], ps[i + 4]);
                i += 4;
            }
        } else if (c >= 30 && c <= 37) pen_.fg = kPal16[c - 30];
        else if (c >= 90 && c <= 97) pen_.fg = kPal16[c - 90 + 8];
        else if (c >= 40 && c <= 47) pen_.bg = kPal16[c - 40];
        else if (c >= 100 && c <= 107) pen_.bg = kPal16[c - 100 + 8];
    }
}

void VtParser::enterAlt() {
    if (alt_) return;
    saveCursor();  // into savedPri_, which ?1049 l restores from
    alt_ = true;
    for (int r = 0; r < rows_; ++r) blankRow(r);
    cx_ = cy_ = 0;
    top_ = 0;
    bot_ = rows_ - 1;
    markAll();
}

void VtParser::leaveAlt() {
    if (!alt_) return;
    alt_ = false;
    top_ = 0;
    bot_ = rows_ - 1;
    restoreCursor();
    markAll();
}

void VtParser::saveCursor() {
    Saved& s = alt_ ? savedAlt_ : savedPri_;
    s = {cx_, cy_, pen_, true};
}

void VtParser::restoreCursor() {
    const Saved& s = alt_ ? savedAlt_ : savedPri_;
    if (!s.valid) return;
    cx_ = s.x;
    cy_ = s.y;
    pen_ = s.pen;
    clampCursor();
}

void VtParser::mode(const QVector<int>& ps, bool set) {
    for (const int p : ps) {
        switch (p) {
            case 1: appCursor_ = set; break;
            case 25: cursorVis_ = set; break;
            case 47:
            case 1047:
            case 1049:
                if (set) enterAlt();
                else leaveAlt();
                break;
            default: break;  // mouse reporting, bracketed paste, ... : not modelled
        }
    }
}

// --- CSI --------------------------------------------------------------------

void VtParser::csiDispatch(char final) {
    // An intermediate byte (e.g. the SP in DECSCUSR "CSI 5 SP q") means this is not
    // one of the sequences we implement; swallow it rather than mistake it for one.
    if (!csiInter_.isEmpty()) return;

    QByteArray p = csiParams_;
    const bool priv = p.startsWith('?');
    if (priv) p = p.mid(1);
    // Sub-parameters are colon-separated in the modern form (38:5:n). Nothing we
    // handle distinguishes ':' from ';', so flatten and parse one list.
    p.replace(':', ';');

    QVector<int> ps;
    if (!p.isEmpty()) {
        for (const QByteArray& tok : p.split(';')) ps.append(tok.isEmpty() ? 0 : tok.toInt());
    }
    const auto arg = [&](int i, int dflt) {
        return (i < ps.size() && ps[i] != 0) ? ps[i] : dflt;
    };
    const int n1 = arg(0, 1);  // "count" params default to 1 when absent or zero

    if (priv) {
        if (final == 'h') mode(ps, true);
        else if (final == 'l') mode(ps, false);
        return;
    }

    switch (final) {
        case 'm': sgr(ps); break;
        case 'H':
        case 'f':
            cy_ = arg(0, 1) - 1;
            cx_ = arg(1, 1) - 1;
            clampCursor();
            break;
        case 'A': cy_ -= n1; clampCursor(); break;
        case 'B': cy_ += n1; clampCursor(); break;
        case 'C': cx_ = cursorX() + n1; clampCursor(); break;
        case 'D': cx_ = cursorX() - n1; clampCursor(); break;
        case 'G': cx_ = arg(0, 1) - 1; clampCursor(); break;
        case 'd': cy_ = arg(0, 1) - 1; clampCursor(); break;
        case 'E': cx_ = 0; cy_ += n1; clampCursor(); break;
        case 'F': cx_ = 0; cy_ -= n1; clampCursor(); break;
        case 'J': eraseDisplay(ps.isEmpty() ? 0 : ps[0]); break;
        case 'K': eraseLine(ps.isEmpty() ? 0 : ps[0]); break;
        case 'L': insertLines(n1); break;
        case 'M': deleteLines(n1); break;
        case 'P': deleteChars(n1); break;
        case '@': insertChars(n1); break;
        case 'X': eraseChars(n1); break;
        case 'S': scrollUp(n1); break;
        case 'T': scrollDown(n1); break;
        case 'r':
            top_ = arg(0, 1) - 1;
            bot_ = arg(1, rows_) - 1;
            top_ = std::clamp(top_, 0, rows_ - 1);
            bot_ = std::clamp(bot_, top_, rows_ - 1);
            cx_ = 0;
            cy_ = top_;
            break;
        case 's': saveCursor(); break;
        case 'u': restoreCursor(); break;
        default: break;
    }
}

// --- UTF-8 ------------------------------------------------------------------

// Incremental decode. A codepoint can straddle a read boundary, so the accumulator
// lives in members; returns true when *out holds a finished codepoint.
bool VtParser::utf8Push(quint8 b, uint* out) {
    if (uniNeed_ > 0) {
        if ((b & 0xC0) == 0x80) {
            uniAcc_ = (uniAcc_ << 6) | (b & 0x3F);
            if (++uniGot_ == uniNeed_) {
                uniNeed_ = uniGot_ = 0;
                *out = uniAcc_;
                return true;
            }
            return false;
        }
        // Truncated: the lead byte promised continuations that never came. Abandon the
        // partial codepoint and reinterpret this byte as a fresh lead — swallowing it
        // instead would eat a perfectly good character.
        uniNeed_ = uniGot_ = 0;
        uniAcc_ = 0;
    }
    if (b < 0x80) {
        *out = b;
        return true;
    }
    if ((b & 0xE0) == 0xC0) {
        uniAcc_ = b & 0x1F;
        uniNeed_ = 1;
        uniGot_ = 0;
        return false;
    }
    if ((b & 0xF0) == 0xE0) {
        uniAcc_ = b & 0x0F;
        uniNeed_ = 2;
        uniGot_ = 0;
        return false;
    }
    if ((b & 0xF8) == 0xF0) {
        uniAcc_ = b & 0x07;
        uniNeed_ = 3;
        uniGot_ = 0;
        return false;
    }
    *out = QChar::ReplacementCharacter;  // stray continuation or 0xFE/0xFF
    return true;
}

// --- the machine ------------------------------------------------------------

void VtParser::feed(const QByteArray& bytes) {
    for (int i = 0; i < bytes.size(); ++i) {
        const quint8 b = quint8(bytes[i]);

        switch (state_) {
            case St::Ground: {
                // An escape or a control byte can never appear mid-codepoint, so if one
                // shows up while we're mid-decode the UTF-8 sequence was truncated —
                // drop it, or its leftover state would eat the next real character.
                if (b == 0x1B) {
                    uniNeed_ = uniGot_ = 0;
                    state_ = St::Esc;
                    continue;
                }
                if (b < 0x20 || b == 0x7F) {
                    uniNeed_ = uniGot_ = 0;
                    switch (b) {
                        case '\r': cx_ = 0; break;
                        case '\n':
                        case 0x0B:  // VT and FF index just like LF on a real terminal
                        case 0x0C: lineFeed(); break;
                        case '\b':
                            if (cx_ > 0) --cx_;
                            break;
                        case '\t': tab(); break;
                        default: break;  // BEL, DEL and the rest: dropped, never drawn as tofu
                    }
                    continue;
                }
                uint cp = 0;
                if (utf8Push(b, &cp)) putChar(cp);
                continue;
            }

            case St::Esc: {
                state_ = St::Ground;
                switch (char(b)) {
                    case '[':
                        csiParams_.clear();
                        csiInter_.clear();
                        state_ = St::Csi;
                        break;
                    case ']':
                    case 'P':  // DCS
                    case '_':  // APC
                    case '^':  // PM
                    case 'X':  // SOS
                        strBuf_.clear();
                        strKind_ = char(b);
                        state_ = St::Str;
                        break;
                    case '(':
                    case ')':
                    case '*':
                    case '+': state_ = St::EscIntermediate; break;
                    case 'M': reverseIndex(); break;
                    case 'D': lineFeed(); break;
                    case 'E':
                        cx_ = 0;
                        lineFeed();
                        break;
                    case '7': saveCursor(); break;
                    case '8': restoreCursor(); break;
                    case 'c': reset(); break;
                    default: break;  // ESC =, ESC >, ESC #8, ... : ignored
                }
                continue;
            }

            case St::EscIntermediate:
                state_ = St::Ground;  // swallow the charset designator ('B', '0', ...)
                continue;

            case St::Csi: {
                if (b >= 0x30 && b <= 0x3F) {
                    csiParams_.append(char(b));
                } else if (b >= 0x20 && b <= 0x2F) {
                    csiInter_.append(char(b));
                } else if (b >= 0x40 && b <= 0x7E) {
                    csiDispatch(char(b));
                    state_ = St::Ground;
                } else if (b == 0x1B) {
                    state_ = St::Esc;  // aborted sequence
                } else if (b < 0x20) {
                    // C0 inside a CSI is executed and the sequence continues; the only
                    // one that matters in practice is CR/LF from a program that got
                    // interrupted mid-escape.
                    if (b == '\r') cx_ = 0;
                    else if (b == '\n') lineFeed();
                } else {
                    state_ = St::Ground;  // garbage: bail out rather than swallow the rest
                }
                continue;
            }

            case St::Str: {
                if (b == 0x07) {  // BEL terminates an OSC
                    state_ = St::Ground;
                    if (strKind_ == ']') {
                        const int semi = strBuf_.indexOf(';');
                        const int code = semi < 0 ? -1 : strBuf_.left(semi).toInt();
                        if (code == 0 || code == 2) {
                            title_ = QString::fromUtf8(strBuf_.mid(semi + 1));
                            titleDirty_ = true;
                        }
                    }
                    strBuf_.clear();
                } else if (b == 0x1B) {
                    state_ = St::StrEsc;
                } else if (strBuf_.size() < 4096) {
                    strBuf_.append(char(b));  // cap: a malformed string must not eat all RAM
                }
                continue;
            }

            case St::StrEsc: {
                if (b == '\\') {  // ST
                    state_ = St::Ground;
                    if (strKind_ == ']') {
                        const int semi = strBuf_.indexOf(';');
                        const int code = semi < 0 ? -1 : strBuf_.left(semi).toInt();
                        if (code == 0 || code == 2) {
                            title_ = QString::fromUtf8(strBuf_.mid(semi + 1));
                            titleDirty_ = true;
                        }
                    }
                    strBuf_.clear();
                } else {
                    // Not a terminator: the ESC belonged to the payload. Keep eating.
                    state_ = St::Str;
                    if (strBuf_.size() < 4096) strBuf_.append(char(0x1B));
                    --i;  // re-run this byte inside Str
                }
                continue;
            }
        }
    }
}

// --- text out ---------------------------------------------------------------

bool VtParser::takeTitle(QString* out) {
    if (!titleDirty_) return false;
    titleDirty_ = false;
    if (out) *out = title_;
    return true;
}

QString VtParser::textAt(int x0, int y0, int x1, int y1) const {
    if (y1 < y0 || (y1 == y0 && x1 < x0)) {
        std::swap(x0, x1);
        std::swap(y0, y1);
    }
    y0 = std::clamp(y0, 0, rows_ - 1);
    y1 = std::clamp(y1, 0, rows_ - 1);

    const QVector<Cell>& scr = activeConst();
    QStringList out;
    for (int r = y0; r <= y1; ++r) {
        const int a = (r == y0) ? std::clamp(x0, 0, cols_ - 1) : 0;
        const int b = (r == y1) ? std::clamp(x1, 0, cols_ - 1) : cols_ - 1;
        QString line;
        for (int c = a; c <= b; ++c) line.append(scr[r * cols_ + c].ch);
        out.append(rstrip(line));
    }
    return out.join(QLatin1Char('\n'));
}

void VtParser::appendHistory(const QString& text) {
    const QStringList lines = QString(text).replace(QLatin1String("\r\n"), QLatin1String("\n")).split(QLatin1Char('\n'));
    for (const QString& l : lines) {
        QVector<Cell> row(cols_);
        for (int c = 0; c < cols_ && c < l.size(); ++c) {
            if (l[c].isPrint()) row[c].ch = l[c];
        }
        sb_.append(row);
        if (sb_.size() > kMaxScrollback) sb_.removeFirst();
    }
    markAll();
}

QString VtParser::dumpText() const {
    QStringList out;
    for (const QVector<Cell>& row : sb_) {
        QString line;
        for (const Cell& c : row) line.append(c.ch);
        out.append(rstrip(line));
    }
    const QVector<Cell>& scr = pri_;  // history is the primary screen's, never the alt's
    for (int r = 0; r < rows_; ++r) {
        QString line;
        for (int c = 0; c < cols_; ++c) line.append(scr[r * cols_ + c].ch);
        out.append(rstrip(line));
    }
    while (!out.isEmpty() && out.last().isEmpty()) out.removeLast();
    return out.join(QLatin1Char('\n'));
}

}  // namespace odv
