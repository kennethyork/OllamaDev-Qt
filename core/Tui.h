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
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <optional>

namespace odv {

// Terminal primitives: size, display width, markdown->ANSI, a raw-mode line
// editor and the collapsing thinking box.
//
// The PHP original shelled out to `stty size` / `tput cols` for every one of
// these. We own the terminal from inside the process, so this file talks to it
// directly (ioctl/termios). That is not a style preference: a fork+exec per
// keystroke-repaint is why the old editor stuttered, and `tput` inherits the
// SHELL's idea of the terminal, not ours.

// ANSI escapes. Kept as plain literals so a caller can build a styled string
// without pulling in a formatting layer.
namespace ansi {
inline constexpr const char* kReset = "\033[0m";
inline constexpr const char* kBold = "\033[1m";
inline constexpr const char* kDim = "\033[2m";
inline constexpr const char* kItalic = "\033[3m";
inline constexpr const char* kRev = "\033[7m";
inline constexpr const char* kRed = "\033[31m";
inline constexpr const char* kGreen = "\033[32m";
inline constexpr const char* kYellow = "\033[33m";
inline constexpr const char* kCyan = "\033[36m";
inline constexpr const char* kClear = "\033[2J\033[H";
}  // namespace ansi

class Tui {
public:
    // Live size from ioctl(TIOCGWINSZ) on the tty, so a mid-session resize is
    // picked up. Falls back to $COLUMNS/$LINES, then 80x24.
    static int width();
    static int height();

    static bool stdoutIsTty();
    static bool stdinIsTty();

    // Display columns the string occupies once printed: ANSI escapes count for
    // nothing, emoji and CJK count 2. wcwidth() does the per-codepoint work (in
    // a UTF-8 locale, which we set ourselves — under LC_ALL=C glibc reports -1
    // for everything above ASCII and every box we draw would be a column short).
    static int visWidth(const QString& s);

    // Width of one codepoint: 0 for control chars, 2 for wide, else 1.
    static int charWidth(char32_t cp);

    static QString stripAnsi(const QString& s);

    // Cut to at most `cols` display columns, appending an ellipsis when cut.
    static QString truncate(const QString& s, int cols);

    // One QString per codepoint (surrogate pairs stay together). The line editor
    // moves the cursor over these, never over QChars: a QChar step would land in
    // the middle of an emoji.
    static QVector<QString> glyphs(const QString& s);
};

// Markdown -> ANSI. Dependency-free and SAFE BY DEFAULT: a line that is not
// recognised markdown is emitted verbatim, so prose, logs and code that merely
// contain markdown-ish punctuation are never corrupted.
class Render {
public:
    // Styled output wanted at all? Off when stdout is not a tty, when NO_COLOR is
    // set, or when ui.markdown=false.
    static bool enabled();

    // No trailing newline is added. Returns the input unchanged on any failure.
    static QString markdown(const QString& text);
};

// Raw-mode line editor drawn as a fixed three-row box:
//
//   ╭─ model · mode · ~/proj ──────╮
//   │ ❯ your text here             │
//   ╰──────────────────────────────╯
//
// The box is EXACTLY three rows no matter how long the input gets — text scrolls
// horizontally inside it — because the repaint climbs back over its own rows with
// cursor-up, and a wrapped row would make that count wrong.
class LineEditor {
public:
    // False for pipes, daemons and hosts that echo keystrokes themselves
    // (OLLAMADEV_SIMPLE_INPUT, e.g. the embedded ADE terminal). The caller then
    // falls back to plain line reads.
    static bool supported();

    struct Completion {
        int start = 0;  // glyph index where the token being completed begins
        QStringList candidates;
    };
    using CompleteFn = std::function<Completion(const QString& line, int cursor)>;

    // Status shown in the top border.
    struct Status {
        QString model;
        QString mode;
        QString cwd;
    };

    // Returns the line; an empty string when Ctrl-C cancelled it; nullopt on EOF
    // (Ctrl-D on an empty line). The termios is restored on every exit path,
    // including a signal that kills us mid-edit.
    static std::optional<QString> readLine(const Status& status, const QStringList& history,
                                           const CompleteFn& complete = {});
};

// A bounded, collapsing "thinking box": streams a model's reasoning dimmed into
// at most N rows, then replaces the whole thing with ` thought for 4s` the
// moment the answer starts.
//
// WHY BOUNDED. A terminal can only erase rows that are still on screen. Once
// reasoning scrolls past the top there is no cursor-up that can reach it, so a
// full-height chain-of-thought could never be folded away. Keeping only the last
// `window` wrapped lines pinned in place means the collapse ALWAYS succeeds.
//
// THE ROW MATH IS THE WHOLE TRICK. Lines are hard-wrapped at width-1 using
// DISPLAY columns (Tui::visWidth), so one emitted line is exactly one physical
// row. If a wrapped line were one column too wide the terminal would soft-wrap it
// into two rows, the cursor-up count would be one short, and the collapse would
// leave a trail of orphaned reasoning behind. Never wrap on QChar count.
// At namespace scope, not nested in Thinking: a nested class's default member
// initializers are not usable from a default argument of the enclosing class
// (the enclosing class is still incomplete there), which is a hard error on g++.
struct ThinkingOptions {
    int cols = 0;    // 0 = terminal width - 1
    int window = 6;  // rows the live box may occupy
    // False for pipes/logs: stream dimmed, but never move the cursor.
    bool control = true;
    QString summaryPrefix;
};

class Thinking {
public:
    using Sink = std::function<void(const QString&)>;
    using Options = ThinkingOptions;

    explicit Thinking(Sink sink, const ThinkingOptions& opt = {});

    void push(const QString& chunk);

    // Erase the box, print the one-line summary. Idempotent; a no-op if nothing
    // was ever streamed.
    void collapse();

    bool shown() const { return shown_; }
    bool done() const { return done_; }

    static QString dur(double seconds);

private:
    void commit();
    void repaint();
    QString rewind() const;

    Sink sink_;
    int cols_;
    int window_;
    bool control_;
    QString summaryPrefix_;

    QVector<QString> buf_;  // completed wrapped lines, at most window_ of them
    QString cur_;           // the line still being filled
    int col_ = 0;           // display columns used by cur_
    int drawn_ = 0;         // rows currently painted
    bool shown_ = false;
    bool done_ = false;
    qint64 startMs_ = 0;
};

}  // namespace odv
