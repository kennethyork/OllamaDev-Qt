// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Terminals.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSocketNotifier>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <cerrno>
#include <csignal>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "Config.h"
#include "Pty.h"

namespace odv {
namespace {

// ---------------------------------------------------------------- the protocol
//
// One unix socket per terminal, framed as: type byte + 4-byte big-endian length +
// payload. Bytes on a pty are arbitrary binary (ANSI, UTF-8 fragments, ^C), so a
// line-based protocol would corrupt them.
constexpr char kFrameIn = 'i';      // client -> host: keystrokes
constexpr char kFrameResize = 'r';  // client -> host: "<cols> <rows>"
constexpr char kFrameQuit = 'q';    // client -> host: terminate the pty and exit
constexpr char kFrameOut = 'o';     // host -> client: pty output
constexpr char kFrameExit = 'x';    // host -> client: the pty exited, code in payload

constexpr int kHeader = 5;
constexpr qint64 kScrollbackBytes = 64 * 1024;   // replayed to a fresh attach
constexpr qint64 kLogCapBytes = 8 * 1024 * 1024; // rotate past this
constexpr qint64 kLogKeepBytes = 1024 * 1024;    // tail kept when rotating
constexpr int kHostStartTimeoutMs = 5000;
constexpr int kStopGraceMs = 2000;
constexpr char kDetachKey = 0x1d;  // Ctrl-] , the telnet convention

QByteArray frame(char type, const QByteArray& payload) {
    const quint32 n = static_cast<quint32>(payload.size());
    QByteArray f;
    f.reserve(kHeader + payload.size());
    f.append(type);
    f.append(static_cast<char>((n >> 24) & 0xff));
    f.append(static_cast<char>((n >> 16) & 0xff));
    f.append(static_cast<char>((n >> 8) & 0xff));
    f.append(static_cast<char>(n & 0xff));
    f.append(payload);
    return f;
}

// Pull complete frames off `buf`, handing each to `fn`. Leaves any partial frame
// in place for the next chunk.
template <typename Fn>
void drainFrames(QByteArray& buf, Fn fn) {
    while (buf.size() >= kHeader) {
        const quint32 n = (static_cast<quint8>(buf[1]) << 24) |
                          (static_cast<quint8>(buf[2]) << 16) |
                          (static_cast<quint8>(buf[3]) << 8) | static_cast<quint8>(buf[4]);
        if (static_cast<quint64>(buf.size()) < kHeader + static_cast<quint64>(n)) return;
        const char type = buf.at(0);
        const QByteArray payload = buf.mid(kHeader, static_cast<qsizetype>(n));
        buf.remove(0, kHeader + static_cast<qsizetype>(n));
        fn(type, payload);
    }
}

// ------------------------------------------------------------------- identity

// An id indexes straight into a directory path and into a socket name, so it is
// validated rather than escaped. No slashes, no "..", nothing that can climb out
// of the terminals directory.
bool validId(const QString& id) {
    static const QRegularExpression rx(QStringLiteral("^[A-Za-z0-9._-]{1,64}$"));
    return rx.match(id).hasMatch() && !id.contains(QLatin1String(".."));
}

// The kernel's start time for a pid, in clock ticks since boot (field 22 of
// /proc/<pid>/stat). This is what makes a recorded pid SAFE to signal: pids are
// recycled, and a stale record whose pid now belongs to someone else's process
// would otherwise have us killing a stranger. The (pid, starttime) pair is unique
// for the life of the machine.
qint64 procStartTicks(qint64 pid) {
    QFile f(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const QByteArray s = f.readAll();
    // The comm field is parenthesised and may itself contain spaces and ')', so
    // fields are counted from the LAST ')'.
    const qsizetype rp = s.lastIndexOf(')');
    if (rp < 0) return 0;
    const QList<QByteArray> t = s.mid(rp + 1).simplified().split(' ');
    // t[0] is field 3 (state), so field 22 (starttime) is t[19].
    if (t.size() < 20) return 0;
    bool ok = false;
    const qint64 v = t.at(19).toLongLong(&ok);
    return ok ? v : 0;
}

QByteArray procCmdline(qint64 pid) {
    QFile f(QStringLiteral("/proc/%1/cmdline").arg(pid));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();  // NUL-separated argv
}

// Is `pid` still alive AND still the process we recorded? Three factors, all
// cheap: it exists, its start time is the one we saw, and its argv is the one we
// spawned. A recycled pid fails at least one. If we cannot PROVE it is ours we
// return false and never signal it — refusing to kill is always the safe error.
bool isOurHost(qint64 pid, qint64 startTicks, const QString& id) {
    if (pid <= 1) return false;
    const qint64 st = procStartTicks(pid);
    if (st == 0) return false;                             // gone, or not ours to read
    if (startTicks != 0 && st != startTicks) return false; // the pid was recycled
    const QByteArray cl = procCmdline(pid);
    if (cl.isEmpty()) return false;
    return cl.contains("__host__") && cl.contains(id.toUtf8());
}

// The pty child (the shell) as recorded by its host. Same rule.
bool isOurShell(qint64 pid, qint64 startTicks) {
    if (pid <= 1 || startTicks == 0) return false;
    return procStartTicks(pid) == startTicks;
}

// --------------------------------------------------------------- the registry

QString baseDir() {
    return Config::terminalsDir();
}

QString sockPath(const QString& id) {
    return Terminals::dirFor(id) + QStringLiteral("/ctl.sock");
}

QString logPath(const QString& id) {
    return Terminals::dirFor(id) + QStringLiteral("/session.log");
}

QString recordPath(const QString& id) {
    return Terminals::dirFor(id) + QStringLiteral("/session.json");
}

QJsonObject readRecord(const QString& id) {
    QFile f(recordPath(id));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument d = QJsonDocument::fromJson(f.readAll());
    return d.isObject() ? d.object() : QJsonObject{};
}

bool writeRecord(const QString& id, const QJsonObject& o) {
    QDir().mkpath(Terminals::dirFor(id));
    // Written by the host and by every CLI that reaps it, so a torn file would be
    // a permanent "terminal not found".
    QSaveFile f(recordPath(id));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return f.commit();
}

TerminalInfo infoFrom(const QString& id, const QJsonObject& o) {
    TerminalInfo t;
    if (o.isEmpty()) return t;
    t.id = o.value(QStringLiteral("id")).toString(id);
    t.cwd = o.value(QStringLiteral("cwd")).toString();
    t.program = o.value(QStringLiteral("program")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("args")).toArray()) t.args << v.toString();
    t.model = o.value(QStringLiteral("model")).toString();
    t.created = o.value(QStringLiteral("created")).toString();
    t.hostPid = static_cast<qint64>(o.value(QStringLiteral("hostPid")).toDouble(-1));
    t.hostStart = static_cast<qint64>(o.value(QStringLiteral("hostStart")).toDouble(0));
    t.shellPid = static_cast<qint64>(o.value(QStringLiteral("shellPid")).toDouble(-1));
    t.shellStart = static_cast<qint64>(o.value(QStringLiteral("shellStart")).toDouble(0));
    t.running = o.value(QStringLiteral("running")).toBool();
    return t;
}

QJsonObject recordFrom(const TerminalInfo& t) {
    QJsonArray args;
    for (const QString& a : t.args) args.append(a);
    return QJsonObject{{QStringLiteral("id"), t.id},
                       {QStringLiteral("cwd"), t.cwd},
                       {QStringLiteral("program"), t.program},
                       {QStringLiteral("args"), args},
                       {QStringLiteral("model"), t.model},
                       {QStringLiteral("created"), t.created},
                       {QStringLiteral("hostPid"), static_cast<double>(t.hostPid)},
                       {QStringLiteral("hostStart"), static_cast<double>(t.hostStart)},
                       {QStringLiteral("shellPid"), static_cast<double>(t.shellPid)},
                       {QStringLiteral("shellStart"), static_cast<double>(t.shellStart)},
                       {QStringLiteral("running"), t.running}};
}

// Reconcile the record with reality: a host that died (crash, reboot, kill -9)
// leaves running=true behind. This is a REAP, not a cleanup — it writes a flag,
// it never signals a process and never deletes a directory.
TerminalInfo reap(const QString& id) {
    TerminalInfo t = infoFrom(id, readRecord(id));
    if (t.isNull()) return t;
    const bool alive = isOurHost(t.hostPid, t.hostStart, t.id);
    if (t.running && !alive) {
        t.running = false;
        t.hostPid = -1;
        t.hostStart = 0;
        t.shellPid = -1;
        t.shellStart = 0;
        writeRecord(id, recordFrom(t));
    } else {
        t.running = alive;
    }
    return t;
}

// Ids THIS process created. The ONLY thing stopOwned() may touch — a second
// instance starts with this empty and so is structurally incapable of stopping
// the first instance's terminals.
QSet<QString>& owned() {
    static QSet<QString> s;
    return s;
}
QMutex& ownedLock() {
    static QMutex m;
    return m;
}
void markOwned(const QString& id) {
    QMutexLocker g(&ownedLock());
    owned().insert(id);
}

// One request on a terminal's socket, connect-write-close. Used by send() and
// broadcast(); an attach keeps its socket open instead.
bool poke(const QString& id, const QByteArray& payload, int waitMs = 1000) {
    QLocalSocket s;
    s.connectToServer(sockPath(id));
    if (!s.waitForConnected(waitMs)) return false;
    s.write(payload);
    const bool ok = s.waitForBytesWritten(waitMs);
    s.disconnectFromServer();
    if (s.state() != QLocalSocket::UnconnectedState) s.waitForDisconnected(waitMs);
    return ok;
}

QString nowIso() {
    return QDateTime::currentDateTime().toString(Qt::ISODate);
}

}  // namespace

// ---------------------------------------------------------------------- basics

QString Terminals::dirFor(const QString& id) {
    return baseDir() + QLatin1Char('/') + id;
}

bool Terminals::exists(const QString& id) {
    return validId(id) && QFileInfo(recordPath(id)).isFile();
}

TerminalInfo Terminals::get(const QString& id) {
    if (!validId(id)) return {};
    return reap(id);
}

QVector<TerminalInfo> Terminals::list() {
    QVector<TerminalInfo> out;
    QDir d(baseDir());
    if (!d.exists()) return out;
    for (const QString& name : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (!validId(name)) continue;
        const TerminalInfo t = reap(name);
        if (!t.isNull()) out.append(t);
    }
    return out;
}

// ------------------------------------------------------------------- lifecycle

TerminalInfo Terminals::create(const QString& id, const QString& cwd, const QString& model,
                               QString* err) {
    return spawn(id, {}, cwd, model, err);  // empty command => the login shell
}

TerminalInfo Terminals::spawn(const QString& id, const QStringList& command, const QString& cwd,
                              const QString& model, QString* err) {
    const auto fail = [&](const QString& m) {
        if (err) *err = m;
        return TerminalInfo{};
    };
    if (!validId(id))
        return fail(QStringLiteral("bad terminal name '%1' (letters, digits, . _ - only)").arg(id));
    if (exists(id))
        return fail(QStringLiteral("terminal '%1' already exists (start / attach / delete it)").arg(id));

    TerminalInfo t;
    t.id = id;
    t.cwd = cwd.isEmpty() ? QDir::currentPath() : cwd;
    t.program = command.value(0);
    t.args = command.mid(1);
    t.model = model.isEmpty() ? Config::str(QStringLiteral("ollama.defaultModel")) : model;
    t.created = nowIso();

    QDir().mkpath(dirFor(id));
    if (!writeRecord(id, recordFrom(t)))
        return fail(QStringLiteral("cannot write %1").arg(recordPath(id)));

    if (!start(id, err)) return {};
    return get(id);
}

bool Terminals::start(const QString& id, QString* err) {
    const auto fail = [&](const QString& m) {
        if (err) *err = m;
        return false;
    };
    if (!exists(id)) return fail(QStringLiteral("no terminal '%1'").arg(id));

    TerminalInfo t = reap(id);
    if (t.running) {
        markOwned(id);  // adopted: we did not spawn it, but we are allowed to stop it
        return true;
    }

    // The host runs OUR OWN binary — applicationFilePath(), never a PATH lookup or
    // a guess. The PHP hunted for the binary in four places and could pick a stale
    // installed copy that did not have the feature it was being asked to run.
    const QString bin = QCoreApplication::applicationFilePath();
    if (bin.isEmpty() || !QFileInfo(bin).isExecutable())
        return fail(QStringLiteral("cannot locate my own binary to host the terminal"));

    QProcess host;
    host.setProgram(bin);
    host.setArguments({QStringLiteral("terminal"), QStringLiteral("__host__"), id});
    host.setWorkingDirectory(t.cwd);
    // The host must not hold our tty: it outlives this command, and an inherited
    // stdin would have it stealing the user's keystrokes. Its own stderr goes to a
    // file so a failed launch is diagnosable instead of invisible.
    host.setStandardInputFile(QProcess::nullDevice());
    host.setStandardOutputFile(QProcess::nullDevice());
    host.setStandardErrorFile(dirFor(id) + QStringLiteral("/host.log"));

    qint64 pid = 0;
    if (!host.startDetached(&pid))
        return fail(QStringLiteral("could not start the terminal host: %1").arg(host.errorString()));

    // Wait for the host to come up: it writes running=true and its own verified
    // pid, then listens. Polling a flag it wrote is how we know it really got the
    // pty open — a pid alone only proves fork() worked.
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < kHostStartTimeoutMs) {
        const TerminalInfo now = infoFrom(id, readRecord(id));
        if (now.running && isOurHost(now.hostPid, now.hostStart, id)) {
            markOwned(id);
            return true;
        }
        QThread::msleep(25);
    }

    QString detail;
    QFile hl(dirFor(id) + QStringLiteral("/host.log"));
    if (hl.open(QIODevice::ReadOnly)) detail = QString::fromUtf8(hl.readAll()).trimmed();
    return fail(detail.isEmpty()
                    ? QStringLiteral("terminal host (pid %1) did not come up").arg(pid)
                    : QStringLiteral("terminal host failed: %1").arg(detail));
}

bool Terminals::stop(const QString& id, QString* err) {
    if (!exists(id)) {
        if (err) *err = QStringLiteral("no terminal '%1'").arg(id);
        return false;
    }
    TerminalInfo t = reap(id);
    if (!t.running) return true;  // already down; reap() has already recorded that

    // 1. Ask it to go quietly: the host terminates its Pty by handle, lets the
    //    shell run its EXIT traps, and exits.
    poke(id, frame(kFrameQuit, {}));

    const auto gone = [&] { return !isOurHost(t.hostPid, t.hostStart, t.id); };
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < kStopGraceMs && !gone()) QThread::msleep(25);

    // 2. Still there: signal the VERIFIED pid. isOurHost() has just proven this pid
    //    is the host we recorded — never a pattern, never a name.
    if (!gone()) {
        ::kill(static_cast<pid_t>(t.hostPid), SIGTERM);
        clock.restart();
        while (clock.elapsed() < kStopGraceMs && !gone()) QThread::msleep(25);
    }
    if (!gone()) ::kill(static_cast<pid_t>(t.hostPid), SIGKILL);

    // 3. Backstop. Killing the host closes the pty master, which hangs up the tty
    //    and SIGHUPs the shell — so this is almost always already true. But a
    //    process that ignores SIGHUP would survive it, and it is OUR recorded
    //    child: verify and take it down by pid, not by name.
    if (isOurShell(t.shellPid, t.shellStart)) {
        ::kill(static_cast<pid_t>(-t.shellPid), SIGHUP);  // the shell's whole group
        QThread::msleep(100);
        if (isOurShell(t.shellPid, t.shellStart)) ::kill(static_cast<pid_t>(-t.shellPid), SIGKILL);
    }

    t.running = false;
    t.hostPid = -1;
    t.hostStart = 0;
    t.shellPid = -1;
    t.shellStart = 0;
    writeRecord(id, recordFrom(t));
    QFile::remove(sockPath(id));

    QMutexLocker g(&ownedLock());
    owned().remove(id);
    return true;
}

bool Terminals::remove(const QString& id, QString* err) {
    if (!validId(id)) {
        if (err) *err = QStringLiteral("bad terminal name '%1'").arg(id);
        return false;
    }
    const QString dir = dirFor(id);
    // Belt and braces: whatever happens above, the thing we are about to delete
    // must be a CHILD of the terminals directory and not the directory itself.
    // (`rm -rf ~/.ollamadev/terminals/*` is the exact bug this port exists to
    // avoid; a removeRecursively() on the wrong QString is how it comes back.)
    if (!QFileInfo(dir).isDir() || QDir(dir) == QDir(baseDir())) {
        if (err) *err = QStringLiteral("no terminal '%1'").arg(id);
        return false;
    }
    stop(id, nullptr);
    if (!QDir(dir).removeRecursively()) {
        if (err) *err = QStringLiteral("could not remove %1").arg(dir);
        return false;
    }
    return true;
}

void Terminals::stopOwned() {
    QStringList ids;
    {
        QMutexLocker g(&ownedLock());
        ids = owned().values();
    }
    for (const QString& id : ids) stop(id, nullptr);
}

// ------------------------------------------------------------------------- io

QString Terminals::log(const QString& id, int lines) {
    if (!validId(id)) return {};
    QFile f(logPath(id));
    if (!f.open(QIODevice::ReadOnly)) return {};

    // Read a bounded tail rather than the whole file: a build running for a day
    // can leave megabytes here and `terminal log` is meant to be instant.
    const qint64 size = f.size();
    const qint64 want = qMin<qint64>(size, kScrollbackBytes * 4);
    if (!f.seek(size - want)) return {};
    const QString text = QString::fromUtf8(f.read(want));

    QStringList rows = text.split(QLatin1Char('\n'));
    if (lines > 0 && rows.size() > lines) rows = rows.mid(rows.size() - lines);
    return rows.join(QLatin1Char('\n'));
}

bool Terminals::send(const QString& id, const QByteArray& data, QString* err) {
    const TerminalInfo t = get(id);
    if (t.isNull()) {
        if (err) *err = QStringLiteral("no terminal '%1'").arg(id);
        return false;
    }
    if (!t.running) {
        if (err) *err = QStringLiteral("terminal '%1' is not running").arg(id);
        return false;
    }
    if (!poke(id, frame(kFrameIn, data))) {
        if (err) *err = QStringLiteral("terminal '%1' did not accept input").arg(id);
        return false;
    }
    return true;
}

int Terminals::broadcast(const QByteArray& data) {
    int n = 0;
    for (const TerminalInfo& t : list()) {
        if (!t.running) continue;
        if (poke(t.id, frame(kFrameIn, data))) ++n;
    }
    return n;
}

// ---------------------------------------------------------------------- attach

int Terminals::attach(const QString& id, QString* err) {
    const auto fail = [&](const QString& m) {
        if (err) *err = m;
        return 1;
    };
    const TerminalInfo t = get(id);
    if (t.isNull()) return fail(QStringLiteral("no terminal '%1'").arg(id));
    if (!t.running) return fail(QStringLiteral("terminal '%1' is not running (terminal start %1)").arg(id));
    if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO))
        return fail(QStringLiteral("attach needs a terminal (use `terminal log` when piping)"));

    QLocalSocket sock;
    sock.connectToServer(sockPath(id));
    if (!sock.waitForConnected(2000))
        return fail(QStringLiteral("cannot reach terminal '%1': %2").arg(id, sock.errorString()));

    // Raw mode: the remote pty already does echo, line editing and signals, so ours
    // must do none of them — a Ctrl-C has to travel to the shell as a byte, not
    // kill this attach.
    struct termios saved {};
    const bool haveTermios = ::tcgetattr(STDIN_FILENO, &saved) == 0;
    if (haveTermios) {
        struct termios raw = saved;
        ::cfmakeraw(&raw);
        ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    const auto restore = [&] {
        if (haveTermios) ::tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    };

    QEventLoop loop;
    int exitCode = 0;

    const auto sendSize = [&sock] {
        struct winsize ws {};
        if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) return;
        sock.write(frame(kFrameResize,
                         QStringLiteral("%1 %2").arg(ws.ws_col).arg(ws.ws_row).toUtf8()));
    };
    sendSize();

    // Poll the size instead of handling SIGWINCH: a signal handler is process-global
    // state, and this runs inside a CLI that already owns SIGINT.
    QTimer sizeTimer;
    QObject::connect(&sizeTimer, &QTimer::timeout, &sock, [&] { sendSize(); });
    sizeTimer.start(400);

    QByteArray inbuf;
    QObject::connect(&sock, &QLocalSocket::readyRead, &sock, [&] {
        inbuf.append(sock.readAll());
        drainFrames(inbuf, [&](char type, const QByteArray& payload) {
            if (type == kFrameOut) {
                const qint64 n = ::write(STDOUT_FILENO, payload.constData(), payload.size());
                (void)n;
            } else if (type == kFrameExit) {
                exitCode = payload.trimmed().toInt();
                loop.quit();
            }
        });
    });
    QObject::connect(&sock, &QLocalSocket::disconnected, &loop, &QEventLoop::quit);

    QSocketNotifier keys(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&keys, &QSocketNotifier::activated, &sock, [&] {
        char buf[4096];
        const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) {
            loop.quit();
            return;
        }
        QByteArray data(buf, static_cast<qsizetype>(n));
        const qsizetype d = data.indexOf(kDetachKey);
        if (d >= 0) {
            if (d > 0) sock.write(frame(kFrameIn, data.left(d)));  // don't lose what came before
            loop.quit();
            return;
        }
        sock.write(frame(kFrameIn, data));
    });

    QTextStream(stdout) << "\r\n[attached to " << id << " — Ctrl-] detaches, the terminal keeps running]\r\n";
    fflush(stdout);
    loop.exec();

    keys.setEnabled(false);
    sock.disconnectFromServer();
    restore();
    QTextStream(stdout) << "\r\n[detached from " << id << "]\r\n";
    fflush(stdout);
    return exitCode;
}

// ------------------------------------------------------------------------ host

namespace {

// The host process. It owns exactly one Pty (a child, by handle) and one unix
// socket, and it is the only thing that ever writes this terminal's log.
class Host : public QObject {
public:
    Host(QString id, TerminalInfo info) : id_(std::move(id)), info_(std::move(info)) {}

    bool run(QString* err) {
        log_.setFileName(logPath(id_));
        if (!log_.open(QIODevice::WriteOnly | QIODevice::Append)) {
            *err = QStringLiteral("cannot open %1").arg(logPath(id_));
            return false;
        }

        // Our OWN socket path for our OWN id. QLocalServer refuses to listen on a
        // leftover file, and a leftover can only exist because a previous host of
        // THIS id died — no other terminal, and no other instance, is touched.
        const QString path = sockPath(id_);
        QLocalServer::removeServer(path);
        connect(&server_, &QLocalServer::newConnection, this, &Host::onClient);
        if (!server_.listen(path)) {
            *err = QStringLiteral("cannot listen on %1: %2").arg(path, server_.errorString());
            return false;
        }

        QString program = info_.program;
        QStringList args = info_.args;
        if (program.isEmpty()) {
            program = defaultShell();
            // Pty::start cannot set argv[0], so the leading-dash login convention is
            // out; -l is how you ask bash/zsh for a login shell by flag.
            if (args.isEmpty()) args << QStringLiteral("-l");
        }

        pty_ = new Pty(this);
        connect(pty_, &Pty::output, this, &Host::onOutput);
        connect(pty_, &Pty::exited, this, &Host::onExited);
        // 120x34 rather than the 80x24 default: an unattached terminal still runs
        // builds and test suites, and their output is what someone reads back later
        // with `terminal log`.
        pty_->resize(120, 34);

        const QStringList env{QStringLiteral("OLLAMADEV_TERMINAL=") + id_,
                              QStringLiteral("OLLAMADEV_MODEL=") + info_.model};
        if (!pty_->start(program, args, info_.cwd, env, err)) return false;

        info_.running = true;
        info_.hostPid = QCoreApplication::applicationPid();
        info_.hostStart = procStartTicks(info_.hostPid);
        info_.shellPid = pty_->pid();
        info_.shellStart = procStartTicks(info_.shellPid);
        writeRecord(id_, recordFrom(info_));

        installSignalPipe();
        return true;
    }

    // A signal must not run Qt code, so it only writes a byte; the event loop does
    // the work. SIGTERM is how `terminal stop` asks a host that is not answering
    // its socket to go, and we still want the shell's EXIT traps to run.
    void installSignalPipe() {
        if (::pipe(sigPipe_) != 0) return;
        ::fcntl(sigPipe_[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(sigPipe_[1], F_SETFD, FD_CLOEXEC);
        sigFd_ = sigPipe_[1];
        sigNotifier_ = new QSocketNotifier(sigPipe_[0], QSocketNotifier::Read, this);
        connect(sigNotifier_, &QSocketNotifier::activated, this, [this] {
            char b;
            const ssize_t n = ::read(sigPipe_[0], &b, 1);
            (void)n;
            shutdown();
        });
        std::signal(SIGTERM, &Host::onSignal);
        std::signal(SIGHUP, &Host::onSignal);
        std::signal(SIGINT, SIG_IGN);  // a Ctrl-C belongs to the shell, not to us
    }

    void shutdown() {
        if (stopping_) return;
        stopping_ = true;
        pty_->terminate();  // SIGHUP, then SIGKILL — our child, by handle
        // If the shell ignores the hangup, do not hang forever holding the socket.
        QTimer::singleShot(3000, this, [this] {
            if (!exited_) {
                pty_->kill();
                onExited(143);  // 128 + SIGTERM
            }
        });
    }

private:
    static int sigFd_;
    static void onSignal(int) {
        if (sigFd_ >= 0) {
            const char b = 1;
            const ssize_t n = ::write(sigFd_, &b, 1);  // async-signal-safe
            (void)n;
        }
    }

    void onClient() {
        while (QLocalSocket* c = server_.nextPendingConnection()) {
            clients_.append(c);
            connect(c, &QLocalSocket::disconnected, this, [this, c] {
                clients_.removeAll(c);
                buffers_.remove(c);
                c->deleteLater();
            });
            connect(c, &QLocalSocket::readyRead, this, [this, c] { onClientData(c); });

            // Replay the recent scrollback so an attach opens on context rather than
            // on a blank screen waiting for the next keystroke.
            QFile f(logPath(id_));
            if (f.open(QIODevice::ReadOnly)) {
                const qint64 size = f.size();
                const qint64 want = qMin<qint64>(size, kScrollbackBytes);
                if (f.seek(size - want)) c->write(frame(kFrameOut, f.read(want)));
            }
        }
    }

    void onClientData(QLocalSocket* c) {
        QByteArray& buf = buffers_[c];
        buf.append(c->readAll());
        drainFrames(buf, [this](char type, const QByteArray& payload) {
            if (type == kFrameIn) {
                pty_->write(payload);
            } else if (type == kFrameResize) {
                const QList<QByteArray> p = payload.simplified().split(' ');
                if (p.size() == 2) pty_->resize(p.at(0).toInt(), p.at(1).toInt());
            } else if (type == kFrameQuit) {
                shutdown();
            }
        });
    }

    void onOutput(const QByteArray& data) {
        log_.write(data);
        log_.flush();  // another process reads this file; a buffered write is invisible
        rotateIfHuge();
        const QByteArray f = frame(kFrameOut, data);
        for (QLocalSocket* c : clients_) c->write(f);
    }

    void rotateIfHuge() {
        if (log_.size() < kLogCapBytes) return;
        log_.close();
        QFile in(logPath(id_));
        QByteArray tail;
        if (in.open(QIODevice::ReadOnly) && in.seek(in.size() - kLogKeepBytes))
            tail = in.readAll();
        in.close();
        QFile out(logPath(id_));
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            out.write(tail);
            out.close();
        }
        log_.open(QIODevice::WriteOnly | QIODevice::Append);
    }

    void onExited(int code) {
        if (exited_) return;
        exited_ = true;

        const QByteArray bye = frame(kFrameExit, QByteArray::number(code));
        for (QLocalSocket* c : clients_) {
            c->write(bye);
            c->flush();
            c->disconnectFromServer();
        }
        log_.flush();
        log_.close();

        info_.running = false;
        info_.hostPid = -1;
        info_.hostStart = 0;
        info_.shellPid = -1;
        info_.shellStart = 0;
        writeRecord(id_, recordFrom(info_));

        server_.close();
        QFile::remove(sockPath(id_));
        QCoreApplication::exit(0);
    }

    QString id_;
    TerminalInfo info_;
    Pty* pty_ = nullptr;
    QFile log_;
    QLocalServer server_;
    QList<QLocalSocket*> clients_;
    QHash<QLocalSocket*, QByteArray> buffers_;
    QSocketNotifier* sigNotifier_ = nullptr;
    int sigPipe_[2] = {-1, -1};
    bool exited_ = false;
    bool stopping_ = false;
};

int Host::sigFd_ = -1;

}  // namespace

int Terminals::hostMain(const QString& id) {
    if (!validId(id)) return 2;
    const TerminalInfo info = infoFrom(id, readRecord(id));
    if (info.isNull()) {
        QTextStream(stderr) << "no terminal record for '" << id << "'\n";
        return 2;
    }

    Host host(id, info);
    QString err;
    if (!host.run(&err)) {
        QTextStream(stderr) << err << "\n";
        return 1;
    }
    return QCoreApplication::exec();
}

}  // namespace odv
