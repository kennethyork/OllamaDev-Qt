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

namespace odv {

// A last-breath crash breadcrumb. When the process dies on a fatal signal
// (SIGSEGV/SIGABRT/SIGBUS/…), we append the signal, a timestamp and a backtrace to
// ~/.ollamadev/last-crash.log, then re-raise so the OS still produces its normal
// crash/core. It exists because "my app vanished mid-run" is otherwise a mystery:
// with this, the next launch can say WHY it went, instead of guessing.
//
// The handler is written to be async-signal-safe — everything it needs (the log
// path, the process name) is captured at install() time, and inside the handler it
// only calls open/write/backtrace_symbols_fd, never malloc or Qt.
namespace Crash {

// Install the fatal-signal handlers. `appName` labels the log line (e.g. "ade").
void install(const QString& appName);

// If the previous run left a crash breadcrumb, return its one-line summary (signal
// + when) and clear it; empty if the last run exited cleanly. Lets the GUI surface
// "last session crashed (SIGSEGV) — your work was restored" on startup.
QString takeLastCrash();

}  // namespace Crash
}  // namespace odv
