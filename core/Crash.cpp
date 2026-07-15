// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Crash.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <csignal>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>

#if defined(__GLIBC__)
#include <execinfo.h>
#define ODV_HAVE_BACKTRACE 1
#endif

#include "Config.h"

namespace odv {
namespace Crash {
namespace {

// Captured at install() so the handler never has to allocate. A fixed buffer,
// because std::string/QString are not async-signal-safe to build in a handler.
char g_logPath[1024] = {0};
char g_label[64] = {0};

// AS-safe: write a C string to a fd, ignoring short writes (best-effort log).
void rawWrite(int fd, const char* s) {
    if (!s) return;
    const size_t n = ::strlen(s);
    ssize_t off = 0;
    while (off < ssize_t(n)) {
        const ssize_t w = ::write(fd, s + off, n - off);
        if (w <= 0) break;
        off += w;
    }
}

const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (segfault)";
        case SIGABRT: return "SIGABRT (abort)";
        case SIGBUS:  return "SIGBUS (bad memory access)";
        case SIGFPE:  return "SIGFPE (arithmetic)";
        case SIGILL:  return "SIGILL (illegal instruction)";
        default:      return "signal";
    }
}

void handler(int sig) {
    const int fd = ::open(g_logPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        rawWrite(fd, "ollamadev-");
        rawWrite(fd, g_label);
        rawWrite(fd, " crashed: ");
        rawWrite(fd, signalName(sig));
        rawWrite(fd, "\nwhen: ");
        // time()/gmtime are not strictly AS-safe, so write the raw epoch seconds —
        // a number is enough to know WHEN, and it needs no allocation.
        char num[24];
        long t = long(::time(nullptr));
        int i = sizeof(num);
        num[--i] = '\0';
        if (t == 0) num[--i] = '0';
        while (t > 0 && i > 0) { num[--i] = char('0' + t % 10); t /= 10; }
        rawWrite(fd, num + i);
        rawWrite(fd, " (epoch)\nbacktrace:\n");
#ifdef ODV_HAVE_BACKTRACE
        void* frames[64];
        const int n = ::backtrace(frames, 64);
        ::backtrace_symbols_fd(frames, n, fd);  // AS-safe, unlike backtrace_symbols
#else
        rawWrite(fd, "  (no backtrace on this build)\n");
#endif
        ::close(fd);
    }
    // Restore the default action and re-raise, so the OS still cores/reports the
    // crash normally — we only wanted to leave a note on the way out.
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

QString logFilePath() {
    return QDir::homePath() + QStringLiteral("/.ollamadev/last-crash.log");
}

}  // namespace

void install(const QString& appName) {
    const QByteArray path = logFilePath().toLocal8Bit();
    ::strncpy(g_logPath, path.constData(), sizeof(g_logPath) - 1);
    const QByteArray label = appName.toLocal8Bit();
    ::strncpy(g_label, label.constData(), sizeof(g_label) - 1);

    QDir().mkpath(QFileInfo(logFilePath()).absolutePath());

    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) ::signal(sig, &handler);
}

QString takeLastCrash() {
    QFile f(logFilePath());
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return {};
    const QByteArray body = f.readAll();
    f.close();
    QFile::remove(logFilePath());  // consumed — a stale note must not nag every launch
    // The first line is the human summary ("ollamadev-ade crashed: SIGSEGV …").
    const int nl = body.indexOf('\n');
    return QString::fromUtf8(nl < 0 ? body : body.left(nl)).trimmed();
}

}  // namespace Crash
}  // namespace odv
