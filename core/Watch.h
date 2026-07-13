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
