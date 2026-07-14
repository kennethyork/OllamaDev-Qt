// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Pty.h"

#include <QFile>
#include <QMap>
#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

// execvp() takes the child environment from the global `environ`, so the child
// assigns to it rather than calling the GNU-only execvpe().
extern char** environ;

namespace odv {
namespace {

// Ceiling on bytes drained per readable event. A runaway child (`yes`, a build
// log) can write faster than we consume, and since the fd is non-blocking the
// read loop would only stop at EAGAIN — which never comes — starving the event
// loop of repaints and keystrokes. The notifier is level-triggered, so whatever
// is left simply re-fires us on the next turn through the loop.
constexpr int kReadBudget = 256 * 1024;

// Same idea for the final drain after the child dies, except here the loop can
// only be kept alive by a *grandchild* still holding the slave open. Bound it so
// a stray daemon writing to the tty can never hang exited().
constexpr int kFinalDrainBudget = 4 * 1024 * 1024;

constexpr int kGraceMs = 2000;  // SIGHUP -> SIGKILL

// Parent environment + our terminal identity + caller overrides, flattened to
// "K=V" entries. Built in the parent because the child cannot allocate.
QList<QByteArray> buildEnv(const QStringList& extra) {
    QMap<QByteArray, QByteArray> vars;
    for (char** e = environ; e && *e; ++e) {
        const QByteArray entry(*e);
        const qsizetype eq = entry.indexOf('=');
        if (eq <= 0) continue;
        vars.insert(entry.left(eq), entry.mid(eq + 1));
    }
    // Forced, not merely defaulted: an app launched from a desktop menu inherits
    // no TERM at all, and a shell that sees TERM=dumb emits zero color codes —
    // the terminal widget would render a correct, entirely grey world.
    vars.insert("TERM", "xterm-256color");
    vars.insert("COLORTERM", "truecolor");
    for (const QString& e : extra) {
        const QByteArray entry = e.toLocal8Bit();
        const qsizetype eq = entry.indexOf('=');
        if (eq <= 0) continue;
        vars.insert(entry.left(eq), entry.mid(eq + 1));  // caller wins, even over TERM
    }

    QList<QByteArray> out;
    out.reserve(vars.size());
    for (auto it = vars.cbegin(); it != vars.cend(); ++it) out.append(it.key() + '=' + it.value());
    return out;
}

}  // namespace

QString defaultShell() {
    const QString shell = qEnvironmentVariable("SHELL");
    // A GUI process can inherit an empty or stale SHELL; bash is the one thing we
    // can assume exists on the platforms this targets.
    if (!shell.isEmpty() && QFile::exists(shell)) return shell;
    return QStringLiteral("/bin/bash");
}

Pty::Pty(QObject* parent) : QObject(parent) {}

Pty::~Pty() {
    if (pid_ > 1) {
        const auto victim = static_cast<pid_t>(pid_);
        ::kill(-victim, SIGHUP);
        ::kill(-victim, SIGKILL);
        // Blocking, but bounded: the child has just been SIGKILLed and cannot
        // ignore it. This is the only way to guarantee we leave no zombie behind,
        // and no exited() is emitted from a destructor.
        int status = 0;
        pid_t r;
        do {
            r = ::waitpid(victim, &status, 0);
        } while (r < 0 && errno == EINTR);
    }
    closeFds();
}

bool Pty::start(const QString& program, const QStringList& args, const QString& cwd,
                const QStringList& env, QString* err) {
    const auto fail = [&](const QString& msg) {
        if (err) *err = msg;
        return false;
    };
    if (isRunning()) return fail(QStringLiteral("pty: already running"));
    if (program.isEmpty()) return fail(QStringLiteral("pty: no program given"));

    // Everything the child touches between fork() and exec() is allocated here,
    // up front. After fork() in a process that has threads (Qt has several), the
    // child may only call async-signal-safe functions — malloc is not one, so a
    // QString->char* conversion down there could deadlock on the allocator lock.
    QList<QByteArray> argStore;
    argStore.append(program.toLocal8Bit());
    for (const QString& a : args) argStore.append(a.toLocal8Bit());
    std::vector<char*> argv;
    argv.reserve(argStore.size() + 1);
    for (QByteArray& a : argStore) argv.push_back(a.data());
    argv.push_back(nullptr);

    QList<QByteArray> envStore = buildEnv(env);
    std::vector<char*> envp;
    envp.reserve(envStore.size() + 1);
    for (QByteArray& e : envStore) envp.push_back(e.data());
    envp.push_back(nullptr);

    const QByteArray cwdBytes = cwd.toLocal8Bit();

    struct winsize ws {};
    ws.ws_col = static_cast<unsigned short>(cols_);
    ws.ws_row = static_cast<unsigned short>(rows_);

    int master = -1;
    const pid_t child = ::forkpty(&master, nullptr, nullptr, &ws);
    if (child < 0) {
        return fail(QStringLiteral("pty: forkpty failed: %1")
                        .arg(QString::fromLocal8Bit(::strerror(errno))));
    }

    if (child == 0) {
        // Child. forkpty() has already called setsid(), made the slave our
        // controlling terminal and dup'd it onto stdin/stdout/stderr, so the
        // shell gets a genuine tty (job control, isatty(), the lot).
        if (!cwdBytes.isEmpty() && ::chdir(cwdBytes.constData()) != 0) {
            // Do not silently fall back to "/": a shell that opens in the wrong
            // directory looks like it worked and quietly ruins whatever runs next.
            static const char msg[] = "ollamadev: cannot enter working directory\n";
            const ssize_t ignored = ::write(STDERR_FILENO, msg, sizeof(msg) - 1);
            (void)ignored;
            ::_exit(127);
        }
        environ = envp.data();
        ::execvp(argv[0], argv.data());
        static const char msg[] = "ollamadev: exec failed\n";
        const ssize_t ignored = ::write(STDERR_FILENO, msg, sizeof(msg) - 1);
        (void)ignored;
        ::_exit(127);  // 127 == "command not found", the shell convention
    }

    // Parent.
    pid_ = child;
    master_ = master;
    exitEmitted_ = false;
    writeBuf_.clear();

    // O_NONBLOCK so the read loop can terminate at EAGAIN instead of parking the
    // GUI thread; CLOEXEC so unrelated processes we spawn later cannot inherit
    // this master and keep the terminal's EOF from ever arriving.
    const int flags = ::fcntl(master_, F_GETFL, 0);
    ::fcntl(master_, F_SETFL, (flags < 0 ? 0 : flags) | O_NONBLOCK);
    ::fcntl(master_, F_SETFD, FD_CLOEXEC);

    readNotifier_ = new QSocketNotifier(master_, QSocketNotifier::Read, this);
    connect(readNotifier_, &QSocketNotifier::activated, this, &Pty::onReadable);

    writeNotifier_ = new QSocketNotifier(master_, QSocketNotifier::Write, this);
    writeNotifier_->setEnabled(false);  // armed only while writeBuf_ has bytes
    connect(writeNotifier_, &QSocketNotifier::activated, this, &Pty::onWritable);

    startExitWatch();
    return true;
}

bool Pty::isRunning() const {
    return pid_ > 0 && !exitEmitted_;
}

qint64 Pty::pid() const {
    return pid_;
}

void Pty::startExitWatch() {
#ifdef SYS_pidfd_open
    // Child death as a file-descriptor event. The alternative — a SIGCHLD handler
    // — is process-global state that a library has no business installing, and it
    // would fight whatever QProcess is doing elsewhere in the app.
    const int pfd = static_cast<int>(::syscall(SYS_pidfd_open, static_cast<pid_t>(pid_), 0));
    if (pfd >= 0) {
        ::fcntl(pfd, F_SETFD, FD_CLOEXEC);
        pidFd_ = pfd;
        exitNotifier_ = new QSocketNotifier(pidFd_, QSocketNotifier::Read, this);
        connect(exitNotifier_, &QSocketNotifier::activated, this, [this] { reap(); });
        return;
    }
#endif
    // Kernel older than 5.3 (or pidfd blocked by a sandbox): degrade to a slow
    // WNOHANG poll. Only *exit* detection loses its edge here — output stays
    // fully event-driven either way.
    reapTimer_ = new QTimer(this);
    reapTimer_->setInterval(100);
    connect(reapTimer_, &QTimer::timeout, this, [this] { reap(); });
    reapTimer_->start();
}

int Pty::drainSome(int budget, bool* eof) {
    if (eof) *eof = false;
    if (master_ < 0) {
        if (eof) *eof = true;
        return 0;
    }

    char buf[8192];
    int total = 0;
    while (total < budget) {
        const ssize_t n = ::read(master_, buf, sizeof(buf));
        if (n > 0) {
            total += static_cast<int>(n);
            emit output(QByteArray(buf, static_cast<qsizetype>(n)));
            continue;
        }
        if (n == 0) {  // EOF
            if (eof) *eof = true;
            break;
        }
        if (errno == EINTR) continue;  // a signal landed mid-read, not an error
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // nothing more for now
        // On Linux a master whose last slave has closed reports EIO, not EOF, once
        // its buffer is empty. That is the child's tty going away, not a failure.
        if (eof) *eof = true;
        break;
    }
    return total;
}

void Pty::onReadable() {
    bool eof = false;
    drainSome(kReadBudget, &eof);
    if (!eof) return;

    // The child's side of the tty is gone. Stop listening: the notifier is
    // level-triggered and an EOF fd is permanently "ready", so leaving it armed
    // would spin the event loop at 100% CPU.
    if (readNotifier_) readNotifier_->setEnabled(false);

    // Usually the child is already waitable and this finishes the job. If it is
    // not (EOF can beat the exit status by a hair), the pidfd/timer watch will
    // call us again — reap() is idempotent.
    reap();
}

void Pty::onWritable() {
    flushWrite();
}

void Pty::write(const QByteArray& data) {
    if (data.isEmpty() || master_ < 0) return;
    writeBuf_.append(data);
    flushWrite();
}

void Pty::flushWrite() {
    while (!writeBuf_.isEmpty() && master_ >= 0) {
        const ssize_t n = ::write(master_, writeBuf_.constData(), static_cast<size_t>(writeBuf_.size()));
        if (n > 0) {
            writeBuf_.remove(0, static_cast<qsizetype>(n));  // partial write: keep the tail
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;  // tty input full
        // EIO/EPIPE: the child is gone, so there is nobody left to type at and the
        // queued keystrokes are meaningless.
        writeBuf_.clear();
        break;
    }
    // Arm the Write notifier only while bytes are queued: a writable fd fires it
    // continuously, so an idle armed notifier is a busy loop.
    if (writeNotifier_) writeNotifier_->setEnabled(!writeBuf_.isEmpty());
}

void Pty::resize(int cols, int rows) {
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    cols_ = cols;
    rows_ = rows;
    if (master_ < 0) return;  // remembered; applied by the next start()

    struct winsize ws {};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    // The kernel raises SIGWINCH in the child's foreground process group by
    // itself. No /proc walking to find the pts, no shelling out to stty.
    ::ioctl(master_, TIOCSWINSZ, &ws);
}

void Pty::terminate() {
    if (pid_ <= 1) return;
    const auto victim = static_cast<pid_t>(pid_);

    // Negative pid targets the child's whole process group. forkpty() made the
    // child a session leader, so its pid is the pgid — signalling the group also
    // takes down whatever it had in the foreground (a hung vim, a sleep 100).
    ::kill(-victim, SIGHUP);

    if (!killTimer_) {
        killTimer_ = new QTimer(this);
        killTimer_->setSingleShot(true);
        connect(killTimer_, &QTimer::timeout, this, [this] {
            if (pid_ > 1) kill();  // still there after the grace period: no more Mr Nice Guy
        });
    }
    killTimer_->start(kGraceMs);
}

void Pty::kill() {
    if (pid_ <= 1) return;
    const auto victim = static_cast<pid_t>(pid_);
    ::kill(-victim, SIGKILL);
    ::kill(victim, SIGKILL);  // in case it escaped its process group
}

void Pty::reap() {
    if (pid_ <= 0 || exitEmitted_) return;

    int status = 0;
    pid_t r;
    do {
        r = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    } while (r < 0 && errno == EINTR);

    if (r == 0) return;                    // still alive: a spurious wakeup
    if (r < 0 && errno != ECHILD) return;  // transient; the watch will call again

    // r > 0 means we reaped it; ECHILD means somebody already did. Either way the
    // child is finished and no zombie remains.
    int code = 0;
    if (r > 0) {
        if (WIFEXITED(status)) {
            code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            code = 128 + WTERMSIG(status);  // shell convention: 130 == SIGINT, etc.
        }
    }
    finish(code);
}

void Pty::finish(int exitCode) {
    if (exitEmitted_) return;
    exitEmitted_ = true;
    pid_ = -1;

    // Drain BEFORE announcing the exit. Bytes the child wrote just before dying
    // are still sitting in the pty's buffer, and a consumer that sees exited()
    // first would render the terminal as closed and drop them —
    // `bash -c 'echo hi; exit 3'` would lose "hi" entirely.
    int drained = 0;
    bool eof = false;
    while (!eof && drained < kFinalDrainBudget) {
        const int n = drainSome(kReadBudget, &eof);
        if (n == 0) break;  // EAGAIN with the writer dead: there is nothing more
        drained += n;
    }

    if (reapTimer_) reapTimer_->stop();
    if (killTimer_) killTimer_->stop();
    closeFds();

    emit exited(exitCode);
}

void Pty::closeFds() {
    // finish() runs inside the exit notifier's own activated() signal, so these
    // cannot be deleted outright — disarm now (which unregisters the fd from the
    // event dispatcher) and let the event loop free them. Any that outlive it are
    // QObject children of `this` and die with us anyway.
    for (QSocketNotifier** n : {&readNotifier_, &writeNotifier_, &exitNotifier_}) {
        if (!*n) continue;
        (*n)->setEnabled(false);
        (*n)->deleteLater();
        *n = nullptr;
    }

    if (master_ >= 0) {
        int r;
        do {
            r = ::close(master_);
        } while (r < 0 && errno == EINTR);
        master_ = -1;
    }
    if (pidFd_ >= 0) {
        int r;
        do {
            r = ::close(pidFd_);
        } while (r < 0 && errno == EINTR);
        pidFd_ = -1;
    }
    writeBuf_.clear();
}

}  // namespace odv
