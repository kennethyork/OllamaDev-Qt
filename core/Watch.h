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

namespace odv {

struct WatchOptions {
    QString task;        // what the agent should do when something changes
    QStringList paths;   // roots to watch; empty => the process cwd
    int intervalSec = 2; // debounce window / polling period
    bool once = false;   // run the task immediately, once, then exit
    int iterations = 8;  // bound on the agent loop per run
    QString backend;
    QString model;
};

// An always-on local agent: re-run a task whenever the source tree changes.
// Continuous agents are only affordable because the compute is local — a metered
// cloud model would bill every tick.
//
//   ollamadev watch "run the tests and fix any failures"
//   ollamadev watch "update the docs" src docs --interval 3
//   ollamadev watch --once "lint"
//
// The PHP polled every root on a timer, which meant a 2-second floor on latency
// and a full recursive stat of the tree on every tick. This uses
// QFileSystemWatcher (inotify) and falls back to polling only when the kernel
// refuses more watches — which a big tree WILL do, since inotify costs one
// descriptor per directory and the default limit is 128 instances / 8192 watches.
class Watch {
public:
    static int run(const WatchOptions& opt);
};

}  // namespace odv
