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
#include <QString>
#include <QStringList>
#include <QVector>

namespace odv {

// One named, long-lived terminal: a real pty running a shell (or one command),
// hosted by a process that outlives the CLI invocation that created it.
struct TerminalInfo {
    QString id;
    QString cwd;
    QString program;   // empty => the user's login shell
    QStringList args;
    QString model;     // which model an agent driving this terminal should use
    QString created;   // ISO-8601
    qint64 hostPid = -1;    // the process that owns the pty
    qint64 hostStart = 0;   // its /proc starttime, captured when we recorded the pid
    qint64 shellPid = -1;   // the pty child itself (the shell)
    qint64 shellStart = 0;
    bool running = false;

    bool isNull() const { return id.isEmpty(); }
};

// The terminal multiplexer.
//
// WHY THERE IS A SHARED DIRECTORY AT ALL, AND WHY IT IS NOT A LANDMINE.
//
// Terminals live in ~/.ollamadev/terminals/<id>/ — a directory several PROCESSES
// can see (the desktop app, any CLI invocation, an agent). That is the whole
// feature: `ollamadev terminal attach build` from a shell must reach the terminal
// the desktop spawned. The PHP had the same directory, and then destroyed the
// feature with the cleanup it hung off it: on every boot the desktop ran
//
//     pkill -9 -f '__pty-daemon__'  ;  rm -rf ~/.ollamadev/terminals/*
//
// which is a PROCESS-GLOBAL kill and a SHARED-DIRECTORY wipe. Opening a second
// window therefore killed the first window's live terminals and deleted their
// scrollback. This port keeps the shared directory and drops the cleanup:
//
//   * Nothing is ever killed by NAME or by PATTERN. A terminal is stopped by its
//     RECORDED pid, and only after that pid is proven to still be the process we
//     started (pid + /proc starttime + the argv we spawned it with — a recycled
//     pid fails all three, so we never signal a stranger's process).
//   * Nothing is ever deleted wholesale. `delete <id>` removes exactly that one
//     id's directory; there is no path here that touches the parent.
//   * Dead entries are REAPED, not killed: list() checks whether each recorded pid
//     is alive and ours, and if not simply marks the record stopped.
//   * stopOwned() — the only bulk operation — stops just the ids THIS process
//     created. A second instance has an empty owned-set and so can do nothing to
//     the first instance's terminals, by construction.
//
// Inside the host process the pty is a plain Pty child held by handle, and the
// host closes the pty master on the way out, which hangs up the tty and takes the
// shell down with it — so even a hard kill of a host leaves no orphan.
class Terminals {
public:
    static QString dirFor(const QString& id);
    static bool exists(const QString& id);
    static TerminalInfo get(const QString& id);

    // Every recorded terminal, with dead entries reaped (marked stopped) first.
    static QVector<TerminalInfo> list();

    // Both spawn a live pty and return once its host is up. `create` runs the
    // user's login shell; `spawn` runs one command. An empty cwd means the
    // process cwd.
    static TerminalInfo create(const QString& id, const QString& cwd, const QString& model,
                               QString* err);
    static TerminalInfo spawn(const QString& id, const QStringList& command, const QString& cwd,
                              const QString& model, QString* err);

    static bool start(const QString& id, QString* err);  // restart a stopped terminal
    static bool stop(const QString& id, QString* err);
    static bool remove(const QString& id, QString* err);  // stop + delete THIS id's dir

    // Raw output as the pty produced it (ANSI included), last `lines` lines.
    static QString log(const QString& id, int lines);

    // Keystrokes into a running terminal. `send` is one; `broadcast` is every
    // running terminal (returns how many took it).
    static bool send(const QString& id, const QByteArray& data, QString* err);
    static int broadcast(const QByteArray& data);

    // Connect this process's tty to the terminal: keystrokes in, output out, until
    // Ctrl-] or the terminal exits. Requires a tty. Returns the exit code.
    static int attach(const QString& id, QString* err);

    // Stop only the terminals this process started. Never touches anyone else's.
    static void stopOwned();

    // The host: owns the pty, appends to session.log, serves attach/broadcast
    // clients over a unix socket. Blocks in its own event loop; this is what
    // `ollamadev terminal __host__ <id>` runs.
    static int hostMain(const QString& id);
};

}  // namespace odv
