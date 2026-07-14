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

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

class QSocketNotifier;
class QTimer;

namespace odv {

// Default interactive shell for this user (SHELL env, else /bin/bash).
QString defaultShell();

// A real pseudo-terminal, owned in-process.
//
// The PHP app faked this: it shelled out to util-linux `script -qfc`, bridged the
// shell through two files (pty-in / pty-out) that the UI polled every 12-150 ms,
// and resized by walking /proc/<pid>/fd/0 to find the /dev/pts/N then invoking
// `stty -F`. Here we hold the master fd, so output is delivered by the event loop
// the instant the kernel has it (no polling, no files) and a resize is one ioctl.
//
// Not thread-safe: like any QObject with socket notifiers, it must live and be
// used on the thread running its event loop.
class Pty : public QObject {
    Q_OBJECT

public:
    explicit Pty(QObject* parent = nullptr);
    ~Pty() override;

    // Spawn `program` with `args` in `cwd` on a fresh pty. `env` entries ("K=V")
    // are added to the child environment. Returns false and sets *err on failure.
    bool start(const QString& program, const QStringList& args, const QString& cwd,
               const QStringList& env = {}, QString* err = nullptr);

    bool isRunning() const;
    qint64 pid() const;

    void write(const QByteArray& data);  // keystrokes -> child stdin
    void resize(int cols, int rows);     // TIOCSWINSZ -> child gets SIGWINCH
    void terminate();                    // SIGHUP, then SIGKILL after a grace period
    void kill();

signals:
    void output(const QByteArray& data);  // raw bytes incl. ANSI, as they arrive
    void exited(int exitCode);            // emitted exactly once per start()

private:
    void onReadable();
    void onWritable();
    void flushWrite();
    void startExitWatch();
    void reap();               // waitpid(WNOHANG); finishes if the child is gone
    void finish(int exitCode); // drains, tears down, emits exited() once
    void closeFds();

    // Reads at most `budget` bytes, emitting output(). Sets *eof when the child's
    // side of the tty is gone. Returns the number of bytes read.
    int drainSome(int budget, bool* eof);

    int master_ = -1;
    int pidFd_ = -1;  // pidfd_open(2) handle; readable once the child is waitable
    qint64 pid_ = -1;
    bool exitEmitted_ = false;

    // Remembered so a resize() issued before start() still sizes the initial pty:
    // the GUI knows its geometry before the shell exists.
    int cols_ = 80;
    int rows_ = 24;

    QByteArray writeBuf_;  // keystrokes the tty was too full to accept yet

    QSocketNotifier* readNotifier_ = nullptr;
    QSocketNotifier* writeNotifier_ = nullptr;
    QSocketNotifier* exitNotifier_ = nullptr;
    QTimer* reapTimer_ = nullptr;  // only on kernels without pidfd
    QTimer* killTimer_ = nullptr;  // SIGHUP -> SIGKILL grace period
};

}  // namespace odv
