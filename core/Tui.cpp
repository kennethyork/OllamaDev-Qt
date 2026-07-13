#include "Tui.h"

#include <QByteArray>
#include <QDateTime>
#include <QRegularExpression>
#include <QStringList>

#include <clocale>
#include <csignal>
#include <cstdio>
#include <cwchar>
#include <sys/ioctl.h>
#include <termios.h>
#include <poll.h>
#include <unistd.h>

#include "Config.h"

namespace odv {
namespace {

int envInt(const char* name) {
    const QByteArray v = qgetenv(name);
    bool ok = false;
    const int n = QString::fromLatin1(v).toInt(&ok);
    return ok ? n : 0;
}

// wcwidth() answers in terms of the process locale. Qt does not set LC_CTYPE for
// us and a C-locale process reports -1 for every non-ASCII codepoint, which would
// silently make every box we draw one column short per emoji. Ask for the user's
// locale once, and fall back to a UTF-8 one when the environment gives us nothing.
void ensureUtf8Ctype() {
    static const bool once = [] {
        if (const char* l = std::setlocale(LC_CTYPE, "")) {
            const QByteArray name(l);
            if (name.contains("UTF-8") || name.contains("utf8")) return true;
        }
        for (const char* candidate : {"C.UTF-8", "en_US.UTF-8", "POSIX.UTF-8"}) {
            if (std::setlocale(LC_CTYPE, candidate)) return true;
        }
        return false;
    }();
    (void)once;
}

// Codepoints wcwidth() may not know about (older glibc, or a stubbornly
// non-UTF-8 locale). These are the ranges that actually show up in model output:
// arrows/box-drawing that stay narrow, and emoji/CJK that are double-width.
bool wideFallback(char32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) ||   // Hangul Jamo
           (cp >= 0x2E80 && cp <= 0xA4CF) ||   // CJK radicals … Yi
           (cp >= 0xAC00 && cp <= 0xD7A3) ||   // Hangul syllables
           (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK compatibility ideographs
           (cp >= 0xFE30 && cp <= 0xFE6F) ||   // CJK compatibility forms
           (cp >= 0xFF00 && cp <= 0xFF60) ||   // fullwidth forms
           (cp >= 0xFFE0 && cp <= 0xFFE6) ||
           (cp >= 0x1F300 && cp <= 0x1FAFF) || // emoji
           (cp >= 0x1F000 && cp <= 0x1F0FF) ||
           (cp >= 0x20000 && cp <= 0x3FFFD);   // CJK extension planes
}

const QRegularExpression& ansiRe() {
    static const QRegularExpression re(QStringLiteral("\033\\[[0-9;?]*[A-Za-z]"));
    return re;
}

QString up(int n) {
    return n > 0 ? QStringLiteral("\033[%1A").arg(n) : QString();
}
QString right(int n) {
    return n > 0 ? QStringLiteral("\033[%1C").arg(n) : QString();
}

void writeOut(const QString& s) {
    const QByteArray b = s.toUtf8();
    ssize_t off = 0;
    while (off < b.size()) {
        const ssize_t n = ::write(STDOUT_FILENO, b.constData() + off, size_t(b.size() - off));
        if (n <= 0) break;  // the terminal went away; nothing useful left to do
        off += n;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Tui
// ---------------------------------------------------------------------------

int Tui::width() {
    struct winsize ws {};
    // Ask the tty itself, not `tput`: tput reads $TERM and the SHELL's notion of
    // the terminal, and it costs a fork+exec on a path we hit on every keystroke.
    for (int fd : {STDOUT_FILENO, STDERR_FILENO, STDIN_FILENO}) {
        if (::ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    }
    const int cols = envInt("COLUMNS");
    return cols > 0 ? cols : 80;
}

int Tui::height() {
    struct winsize ws {};
    for (int fd : {STDOUT_FILENO, STDERR_FILENO, STDIN_FILENO}) {
        if (::ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
    }
    const int rows = envInt("LINES");
    return rows > 0 ? rows : 24;
}

bool Tui::stdoutIsTty() { return ::isatty(STDOUT_FILENO) == 1; }
bool Tui::stdinIsTty() { return ::isatty(STDIN_FILENO) == 1; }

int Tui::charWidth(char32_t cp) {
    if (cp == 0) return 0;
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;  // control: prints nothing
    ensureUtf8Ctype();
    const int w = ::wcwidth(static_cast<wchar_t>(cp));
    if (w >= 0) return w;
    // wcwidth has no opinion (unassigned to IT, or a non-UTF-8 locale we could not
    // escape). Guessing 1 for an emoji is exactly the off-by-one that breaks the
    // thinking box's cursor-up count, so guess from the range instead.
    return wideFallback(cp) ? 2 : 1;
}

int Tui::visWidth(const QString& s) {
    const QString plain = stripAnsi(s);
    int w = 0;
    for (const char32_t cp : plain.toStdU32String()) w += charWidth(cp);
    return w;
}

QString Tui::stripAnsi(const QString& s) {
    QString out = s;
    return out.remove(ansiRe());
}

QString Tui::truncate(const QString& s, int cols) {
    if (cols <= 0) return {};
    if (visWidth(s) <= cols) return s;
    QString out;
    int w = 0;
    for (const QString& g : glyphs(stripAnsi(s))) {
        const int gw = visWidth(g);
        if (w + gw > cols - 1) break;
        out += g;
        w += gw;
    }
    return out + QStringLiteral("…");
}

QVector<QString> Tui::glyphs(const QString& s) {
    QVector<QString> out;
    out.reserve(s.size());
    for (const char32_t cp : s.toStdU32String())
        out.append(QString::fromUcs4(&cp, 1));
    return out;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

namespace {

const QRegularExpression& fenceRe() {
    static const QRegularExpression re(QStringLiteral("^\\s*(```|~~~)"));
    return re;
}
const QRegularExpression& headingRe() {
    static const QRegularExpression re(QStringLiteral("^\\s*(#{1,6})\\s+(.*)$"));
    return re;
}
const QRegularExpression& hrRe() {
    static const QRegularExpression re(QStringLiteral("^\\s*([-*_])(\\s*\\1){2,}\\s*$"));
    return re;
}
const QRegularExpression& bulletRe() {
    static const QRegularExpression re(QStringLiteral("^(\\s*)[-*+]\\s+(.*)$"));
    return re;
}
const QRegularExpression& numberedRe() {
    static const QRegularExpression re(QStringLiteral("^(\\s*)(\\d+)[.)]\\s+(.*)$"));
    return re;
}
const QRegularExpression& quoteRe() {
    static const QRegularExpression re(QStringLiteral("^\\s*>\\s?(.*)$"));
    return re;
}
const QRegularExpression& keywordRe() {
    static const QRegularExpression re(QStringLiteral(
        "\\b(function|class|struct|namespace|return|if|else|for|while|switch|case|public|private|"
        "protected|static|const|constexpr|auto|var|let|def|import|from|new|delete|using|echo|print|"
        "true|false|null|nullptr|void|int|string|bool|float|double|array)\\b"));
    return re;
}

// Inline spans. Inline code is lifted out FIRST and parked behind a placeholder
// so emphasis markers inside it are never reinterpreted (`a * b` must stay
// literal). Unmatched markers are left exactly as the model wrote them.
QString inlineSpans(const QString& in) {
    QString s = in;
    QStringList codes;

    static const QRegularExpression codeRe(QStringLiteral("`([^`]+)`"));
    QString staged;
    int last = 0;
    auto it = codeRe.globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next();
        staged += s.mid(last, m.capturedStart() - last);
        staged += QStringLiteral("\x01%1\x02").arg(codes.size());
        codes << (QString::fromUtf8(ansi::kRev) + QLatin1Char(' ') + m.captured(1) +
                  QLatin1Char(' ') + QString::fromUtf8(ansi::kReset));
        last = m.capturedEnd();
    }
    staged += s.mid(last);
    s = staged;

    const QString b = QString::fromUtf8(ansi::kBold);
    const QString i = QString::fromUtf8(ansi::kItalic);
    const QString r = QString::fromUtf8(ansi::kReset);

    static const QRegularExpression bold1(QStringLiteral("\\*\\*([^*\n]+?)\\*\\*"));
    static const QRegularExpression bold2(QStringLiteral("__([^_\n]+?)__"));
    static const QRegularExpression ital1(QStringLiteral("(?<![*\\w])\\*([^*\n]+?)\\*(?![*\\w])"));
    static const QRegularExpression ital2(QStringLiteral("(?<![_\\w])_([^_\n]+?)_(?![_\\w])"));
    s.replace(bold1, b + QStringLiteral("\\1") + r);
    s.replace(bold2, b + QStringLiteral("\\1") + r);
    s.replace(ital1, i + QStringLiteral("\\1") + r);
    s.replace(ital2, i + QStringLiteral("\\1") + r);

    for (int n = 0; n < codes.size(); ++n)
        s.replace(QStringLiteral("\x01%1\x02").arg(n), codes.at(n));
    return s;
}

QString renderCode(const QStringList& code) {
    const QString dim = QString::fromUtf8(ansi::kDim);
    const QString reset = QString::fromUtf8(ansi::kReset);
    const QString bar = dim + QStringLiteral("│ ") + reset;
    QStringList out;
    for (const QString& line : code) {
        QString hi = line;
        // Tint keywords, then drop straight back into dim so the rest of the line
        // keeps the code block's colour.
        hi.replace(keywordRe(), QString::fromUtf8(ansi::kGreen) + QStringLiteral("\\1") + dim);
        out << bar + dim + hi + reset;
    }
    return out.join(QLatin1Char('\n'));
}

}  // namespace

bool Render::enabled() {
    if (!Config::boolean(QStringLiteral("ui.markdown"), true)) return false;
    if (!qEnvironmentVariableIsEmpty("NO_COLOR")) return false;
    return Tui::stdoutIsTty();
}

QString Render::markdown(const QString& text) {
    QString src = text;
    src.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    src.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    const QStringList lines = src.split(QLatin1Char('\n'));
    const QString dim = QString::fromUtf8(ansi::kDim);
    const QString cyan = QString::fromUtf8(ansi::kCyan);
    const QString bold = QString::fromUtf8(ansi::kBold);
    const QString reset = QString::fromUtf8(ansi::kReset);

    QStringList out;
    for (int i = 0; i < lines.size(); ++i) {
        const QString& line = lines.at(i);

        const auto fence = fenceRe().match(line);
        if (fence.hasMatch()) {
            const QString marker = fence.captured(1);
            QStringList code;
            bool closed = false;
            int j = i + 1;
            for (; j < lines.size(); ++j) {
                if (lines.at(j).trimmed() == marker) {
                    closed = true;
                    break;
                }
                code << lines.at(j);
            }
            i = closed ? j : lines.size();  // unterminated fence: the rest is code
            out << renderCode(code);
            continue;
        }

        const auto heading = headingRe().match(line);
        if (heading.hasMatch()) {
            out << cyan + bold + inlineSpans(heading.captured(2).trimmed()) + reset;
            continue;
        }
        if (hrRe().match(line).hasMatch()) {
            out << dim + QString(40, QLatin1Char('-')) + reset;
            continue;
        }
        const auto bullet = bulletRe().match(line);
        if (bullet.hasMatch()) {
            out << bullet.captured(1) + cyan + QStringLiteral("• ") + reset +
                       inlineSpans(bullet.captured(2));
            continue;
        }
        const auto numbered = numberedRe().match(line);
        if (numbered.hasMatch()) {
            out << numbered.captured(1) + cyan + numbered.captured(2) + QStringLiteral(". ") +
                       reset + inlineSpans(numbered.captured(3));
            continue;
        }
        const auto quote = quoteRe().match(line);
        if (quote.hasMatch()) {
            out << dim + QStringLiteral("│ ") + reset + inlineSpans(quote.captured(1));
            continue;
        }

        // Not markdown we know: emit it exactly as it came in.
        out << inlineSpans(line);
    }
    return out.join(QLatin1Char('\n'));
}

// ---------------------------------------------------------------------------
// Raw mode
// ---------------------------------------------------------------------------

namespace {

// The saved termios lives at file scope because a SIGNAL has to be able to put
// the terminal back: a handler cannot reach into a stack object. It is written
// exactly once (on entry) and read by the handler, which does nothing but
// tcsetattr + re-raise — both async-signal-safe.
struct termios g_saved {};
volatile sig_atomic_t g_rawActive = 0;

void restoreOnSignal(int sig) {
    if (g_rawActive) {
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
        g_rawActive = 0;
        // Leave the cursor on a fresh line rather than inside a half-drawn box.
        const char nl[] = "\r\n";
        ssize_t ignored = ::write(STDOUT_FILENO, nl, sizeof(nl) - 1);
        (void)ignored;
    }
    // Re-raise with the default disposition so the process dies the way the user
    // asked it to. Swallowing the signal here would make Ctrl-\ / SIGTERM feel
    // broken.
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

// RAII, and it MUST be: every exit path out of the editor — return, exception,
// SIGTERM — has to hand the terminal back cooked, or the user's shell is left
// with no echo and no line discipline.
class RawMode {
public:
    RawMode() {
        if (::tcgetattr(STDIN_FILENO, &saved_) != 0) return;
        g_saved = saved_;

        struct termios raw = saved_;
        raw.c_lflag &= ~tcflag_t(ECHO | ICANON | ISIG | IEXTEN);
        raw.c_iflag &= ~tcflag_t(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
        raw.c_oflag &= ~tcflag_t(OPOST);  // we emit \r\n ourselves, everywhere
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        // TCSADRAIN, never TCSAFLUSH: flushing would DISCARD anything typed before
        // the prompt appeared, so every character you type while the model is still
        // answering would vanish the moment the next prompt opens.
        if (::tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) != 0) return;

        ok_ = true;
        g_rawActive = 1;
        for (int sig : {SIGINT, SIGTERM, SIGQUIT, SIGHUP})
            prev_[sigIndex(sig)] = ::signal(sig, restoreOnSignal);
    }

    ~RawMode() {
        if (!ok_) return;
        // TCSADRAIN again: bytes already typed belong to the NEXT prompt, not to the
        // bit bucket.
        ::tcsetattr(STDIN_FILENO, TCSADRAIN, &saved_);
        g_rawActive = 0;
        for (int sig : {SIGINT, SIGTERM, SIGQUIT, SIGHUP})
            ::signal(sig, prev_[sigIndex(sig)]);
    }

    RawMode(const RawMode&) = delete;
    RawMode& operator=(const RawMode&) = delete;

    bool ok() const { return ok_; }

private:
    static int sigIndex(int sig) {
        switch (sig) {
            case SIGINT: return 0;
            case SIGTERM: return 1;
            case SIGQUIT: return 2;
            default: return 3;  // SIGHUP
        }
    }

    struct termios saved_ {};
    bool ok_ = false;
    void (*prev_[4])(int) = {SIG_DFL, SIG_DFL, SIG_DFL, SIG_DFL};
};

enum class Key {
    Char, Enter, Backspace, Delete, Left, Right, Home, End, Up, Down, Tab,
    CtrlC, Eof, KillLine, Ignore
};

struct Press {
    Key key = Key::Ignore;
    QString text;
};

// Blocking single byte. Returns false on EOF.
bool readByte(char* out, int timeoutMs = -1) {
    if (timeoutMs >= 0) {
        struct pollfd p {STDIN_FILENO, POLLIN, 0};
        if (::poll(&p, 1, timeoutMs) <= 0) return false;
    }
    const ssize_t n = ::read(STDIN_FILENO, out, 1);
    return n == 1;
}

Press readKey() {
    char b = 0;
    if (!readByte(&b)) return {Key::Eof, {}};
    const unsigned char o = static_cast<unsigned char>(b);

    switch (o) {
        case 13: case 10: return {Key::Enter, {}};
        case 127: case 8: return {Key::Backspace, {}};
        case 3: return {Key::CtrlC, {}};
        case 4: return {Key::Eof, {}};
        case 1: return {Key::Home, {}};
        case 5: return {Key::End, {}};
        case 9: return {Key::Tab, {}};
        case 11: return {Key::KillLine, {}};  // Ctrl-K
        case 27: {
            // A bare Esc must not hang us waiting for a sequence that never comes,
            // so the continuation bytes are polled, not blocked on.
            char n1 = 0;
            if (!readByte(&n1, 50)) return {Key::Ignore, {}};
            if (n1 != '[' && n1 != 'O') return {Key::Ignore, {}};
            QByteArray seq;
            while (true) {
                char x = 0;
                if (!readByte(&x, 50)) break;
                seq.append(x);
                const unsigned char c = static_cast<unsigned char>(x);
                if (c >= 0x40 && c <= 0x7E) break;  // final byte of a CSI sequence
            }
            if (seq == "A") return {Key::Up, {}};
            if (seq == "B") return {Key::Down, {}};
            if (seq == "C") return {Key::Right, {}};
            if (seq == "D") return {Key::Left, {}};
            if (seq == "H" || seq == "1~" || seq == "7~") return {Key::Home, {}};
            if (seq == "F" || seq == "4~" || seq == "8~") return {Key::End, {}};
            if (seq == "3~") return {Key::Delete, {}};
            return {Key::Ignore, {}};
        }
        default: break;
    }
    if (o < 32) return {Key::Ignore, {}};

    // UTF-8: pull the continuation bytes so a multi-byte character arrives whole.
    // Reading one byte at a time and appending it to the buffer would corrupt every
    // non-ASCII keystroke.
    QByteArray ch(1, b);
    const int extra = o >= 0xF0 ? 3 : (o >= 0xE0 ? 2 : (o >= 0xC0 ? 1 : 0));
    for (int i = 0; i < extra; ++i) {
        char x = 0;
        if (!readByte(&x, 50)) break;
        ch.append(x);
    }
    return {Key::Char, QString::fromUtf8(ch)};
}

// The three-row input box. Owns where the cursor is, which is the only thing
// that makes an in-place repaint safe.
class Box {
public:
    void paint(const QVector<QString>& glyphs, int cursor, const LineEditor::Status& st,
               bool placeCursor) {
        const int w = qBound(30, Tui::width(), 200);
        const int inner = w - 4;          // between "│ " and " │"
        const int avail = inner - 2;      // minus the "❯ " prompt

        if (cursor < start_) start_ = cursor;
        if (start_ > glyphs.size()) start_ = glyphs.size();
        while (start_ < cursor && widthOf(glyphs, start_, cursor) > avail) ++start_;

        QString visible;
        int used = 0;
        for (int i = start_; i < glyphs.size(); ++i) {
            const int gw = Tui::visWidth(glyphs.at(i));
            if (used + gw > avail) break;
            visible += glyphs.at(i);
            used += gw;
        }

        const QString dim = QString::fromUtf8(ansi::kDim);
        const QString cyan = QString::fromUtf8(ansi::kCyan);
        const QString reset = QString::fromUtf8(ansi::kReset);

        QString status = st.model;
        if (!st.mode.isEmpty()) status += QStringLiteral(" · ") + st.mode;
        if (!st.cwd.isEmpty()) status += QStringLiteral(" · ") + st.cwd;
        status = Tui::truncate(status, qMax(4, w - 8));

        const int lead = Tui::visWidth(QStringLiteral("╭─ ")) + Tui::visWidth(status) + 1;
        const int fill = qMax(0, w - 1 - lead);
        const QString top = dim + QStringLiteral("╭─ ") + reset + dim + status + QLatin1Char(' ') +
                            QString(fill, QChar(u'─')) + QStringLiteral("╮") + reset;
        const QString mid = dim + QStringLiteral("│ ") + reset + cyan + QStringLiteral("❯ ") +
                            reset + visible + QString(qMax(0, avail - used), QLatin1Char(' ')) +
                            dim + QStringLiteral(" │") + reset;
        const QString bot = dim + QStringLiteral("╰") + QString(w - 2, QChar(u'─')) +
                            QStringLiteral("╯") + reset;

        QString seq = rewind();
        // OPOST is off in raw mode, so "\n" would drop a row without returning to
        // column 0 and every border would start where the last one ended.
        seq += top + QStringLiteral("\r\n") + mid + QStringLiteral("\r\n") + bot;
        drawn_ = 3;

        if (placeCursor) {
            const int col = 4 + widthOf(glyphs, start_, cursor);  // "│ " + "❯ "
            seq += QStringLiteral("\r") + up(1) + right(col);
            cursorRow_ = 1;
        } else {
            cursorRow_ = 2;  // parked at the end of the bottom border
        }
        writeOut(seq);
    }

    // Erase the box entirely and forget it, so the next paint draws a fresh one
    // below whatever the caller prints in between (e.g. a completion list).
    void erase() {
        if (drawn_ <= 0) return;
        writeOut(rewind());
        drawn_ = 0;
        cursorRow_ = 0;
    }

    void reset() { start_ = 0; }

private:
    QString rewind() const {
        if (drawn_ <= 0) return {};
        return QStringLiteral("\r") + up(cursorRow_) + QStringLiteral("\033[J");
    }

    static int widthOf(const QVector<QString>& g, int from, int to) {
        int w = 0;
        for (int i = qMax(0, from); i < qMin(to, int(g.size())); ++i) w += Tui::visWidth(g.at(i));
        return w;
    }

    int start_ = 0;      // first visible glyph (horizontal scroll)
    int drawn_ = 0;      // rows currently on screen
    int cursorRow_ = 0;  // where we left the cursor, 0-based within the box
};

QString commonPrefix(const QStringList& strs) {
    if (strs.isEmpty()) return {};
    QVector<QString> first = Tui::glyphs(strs.first());
    int len = first.size();
    for (const QString& s : strs) {
        const QVector<QString> g = Tui::glyphs(s);
        len = qMin(len, int(g.size()));
        for (int i = 0; i < len; ++i) {
            if (g.at(i) != first.at(i)) {
                len = i;
                break;
            }
        }
    }
    QString out;
    for (int i = 0; i < len; ++i) out += first.at(i);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// LineEditor
// ---------------------------------------------------------------------------

bool LineEditor::supported() {
    // A host that echoes keystrokes itself (the ADE's embedded terminal) would
    // double every character and fight our cursor control.
    if (!qEnvironmentVariableIsEmpty("OLLAMADEV_SIMPLE_INPUT")) return false;
    return Tui::stdinIsTty() && Tui::stdoutIsTty();
}

std::optional<QString> LineEditor::readLine(const Status& status, const QStringList& history,
                                            const CompleteFn& complete) {
    RawMode raw;
    if (!raw.ok()) return std::nullopt;

    Box box;
    QVector<QString> glyphs;
    int cursor = 0;
    int hidx = history.size();
    QString stash;
    std::optional<QString> result;

    box.paint(glyphs, cursor, status, true);

    bool running = true;
    while (running) {
        const Press p = readKey();
        switch (p.key) {
            case Key::Enter: {
                QString line;
                for (const QString& g : glyphs) line += g;
                result = line;
                running = false;
                break;
            }
            case Key::Eof:
                if (glyphs.isEmpty()) {
                    result = std::nullopt;
                    running = false;
                }
                break;  // Ctrl-D mid-line does nothing, as in every other shell
            case Key::CtrlC:
                // Raw mode means Ctrl-C is a byte, not SIGINT: cancel the LINE, do
                // not kill the process (that would drop the whole session).
                result = QString();
                running = false;
                break;
            case Key::Char:
                glyphs.insert(cursor, p.text);
                ++cursor;
                break;
            case Key::Backspace:
                if (cursor > 0) {
                    glyphs.remove(cursor - 1);
                    --cursor;
                }
                break;
            case Key::Delete:
                if (cursor < glyphs.size()) glyphs.remove(cursor);
                break;
            case Key::KillLine:
                while (glyphs.size() > cursor) glyphs.remove(glyphs.size() - 1);
                break;
            case Key::Left:
                if (cursor > 0) --cursor;
                break;
            case Key::Right:
                if (cursor < glyphs.size()) ++cursor;
                break;
            case Key::Home:
                cursor = 0;
                break;
            case Key::End:
                cursor = glyphs.size();
                break;
            case Key::Up:
                if (hidx > 0) {
                    if (hidx == history.size()) {
                        stash.clear();
                        for (const QString& g : glyphs) stash += g;
                    }
                    --hidx;
                    glyphs = Tui::glyphs(history.at(hidx));
                    cursor = glyphs.size();
                }
                break;
            case Key::Down:
                if (hidx < history.size()) {
                    ++hidx;
                    glyphs = Tui::glyphs(hidx == history.size() ? stash : history.at(hidx));
                    cursor = glyphs.size();
                }
                break;
            case Key::Tab: {
                if (!complete) break;
                QString line;
                for (const QString& g : glyphs) line += g;
                const Completion c = complete(line, cursor);
                if (c.candidates.isEmpty()) break;
                const int tstart = qBound(0, c.start, cursor);
                QString token;
                for (int i = tstart; i < cursor; ++i) token += glyphs.at(i);

                auto replace = [&](const QString& with) {
                    const QVector<QString> g = Tui::glyphs(with);
                    for (int i = cursor - 1; i >= tstart; --i) glyphs.remove(i);
                    for (int i = 0; i < g.size(); ++i) glyphs.insert(tstart + i, g.at(i));
                    cursor = tstart + g.size();
                };

                if (c.candidates.size() == 1) {
                    replace(c.candidates.first());
                    break;
                }
                const QString common = commonPrefix(c.candidates);
                if (common.size() > token.size()) {
                    replace(common);
                    break;
                }
                // Nothing more to share: show the options above a freshly drawn box.
                box.erase();
                const QString dim = QString::fromUtf8(ansi::kDim);
                const QString reset = QString::fromUtf8(ansi::kReset);
                writeOut(dim + c.candidates.join(QStringLiteral("   ")) + reset +
                         QStringLiteral("\r\n"));
                break;
            }
            case Key::Ignore:
                break;
        }
        if (running) box.paint(glyphs, cursor, status, true);
    }

    // Repaint one last time WITHOUT reclaiming the cursor, so the finished box is
    // left intact in the scrollback and the next write starts under it.
    box.paint(glyphs, cursor, status, false);
    writeOut(QStringLiteral("\r\n"));
    return result;
}

// ---------------------------------------------------------------------------
// Thinking
// ---------------------------------------------------------------------------

Thinking::Thinking(Sink sink, const ThinkingOptions& opt)
    : sink_(std::move(sink)),
      cols_(opt.cols > 0 ? opt.cols : qMax(20, Tui::width() - 1)),
      control_(opt.control),
      summaryPrefix_(opt.summaryPrefix),
      startMs_(QDateTime::currentMSecsSinceEpoch()) {
    // The box can never be taller than the screen: its own collapse walks the
    // cursor back over every row it drew, and cursor-up cannot climb past the top
    // of the viewport.
    window_ = qBound(1, opt.window, qMax(1, Tui::height() - 2));
}

void Thinking::push(const QString& chunk) {
    if (chunk.isEmpty()) return;
    shown_ = true;
    if (!control_) {
        // Piped or logged: no cursor control is legal here, so just dim the text.
        sink_(QString::fromUtf8(ansi::kDim) + chunk + QString::fromUtf8(ansi::kReset));
        return;
    }

    for (const char32_t cp : chunk.toStdU32String()) {
        if (cp == U'\r') continue;
        if (cp == U'\n') {
            commit();
            continue;
        }
        const QString g = cp == U'\t' ? QStringLiteral(" ") : QString::fromUcs4(&cp, 1);
        const int w = Tui::charWidth(cp == U'\t' ? U' ' : cp);
        // Hard wrap on DISPLAY columns. An emoji that would straddle the edge must
        // wrap early: letting the terminal soft-wrap it costs us a phantom row and
        // the collapse would then leave a stripe of orphaned reasoning behind.
        if (col_ + w > cols_) commit();
        cur_ += g;
        col_ += w;
    }
    repaint();
}

void Thinking::commit() {
    buf_.append(cur_);
    while (buf_.size() > window_) buf_.removeFirst();
    cur_.clear();
    col_ = 0;
}

void Thinking::repaint() {
    QVector<QString> vis = buf_;
    if (!cur_.isEmpty() || vis.isEmpty()) vis.append(cur_);
    while (vis.size() > window_) vis.removeFirst();

    QString seq = rewind();
    QStringList body;
    for (const QString& l : vis)
        body << QString::fromUtf8(ansi::kDim) + l + QString::fromUtf8(ansi::kReset);
    seq += body.join(QLatin1Char('\n'));
    drawn_ = vis.size();
    sink_(seq);
}

QString Thinking::rewind() const {
    if (drawn_ <= 0) return {};
    // The cursor sits at the END of the last painted row, so climb drawn_-1 rows —
    // not drawn_. One row too many and the collapse eats the line above the box.
    return QStringLiteral("\r") + up(drawn_ - 1) + QStringLiteral("\033[J");
}

void Thinking::collapse() {
    if (done_ || !shown_) {
        done_ = true;
        return;
    }
    done_ = true;
    const double secs = double(QDateTime::currentMSecsSinceEpoch() - startMs_) / 1000.0;
    if (!control_) {
        sink_(QStringLiteral("\n"));
        return;
    }
    const QString label = QString::fromUtf8(ansi::kDim) + QStringLiteral("💭 thought for ") +
                          dur(secs) + QString::fromUtf8(ansi::kReset);
    sink_(rewind() + summaryPrefix_ + label + QStringLiteral("\n"));
    drawn_ = 0;
}

QString Thinking::dur(double seconds) {
    if (seconds < 1) return QStringLiteral("%1s").arg(seconds, 0, 'f', 1);
    if (seconds < 60) return QStringLiteral("%1s").arg(qRound(seconds));
    const int m = int(seconds) / 60;
    const int r = qRound(seconds - m * 60);
    return r ? QStringLiteral("%1m %2s").arg(m).arg(r) : QStringLiteral("%1m").arg(m);
}

}  // namespace odv
