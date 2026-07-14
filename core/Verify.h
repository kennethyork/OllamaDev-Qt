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
#include <functional>
#include <optional>

#include "Backend.h"

namespace odv {

// How this project runs its tests. `cmd` is a command LINE (it may contain flags
// and, from a config override, shell syntax); `label` names the runner we matched
// so the user can see what we guessed and correct it.
struct TestCommand {
    QString cmd;
    QString label;
};

struct TestRun {
    QString cmd;
    QString label;
    int exit = -1;
    QString output;  // stdout + stderr, merged and in order
    bool green() const { return exit == 0; }
};

// Progress for a fix loop. Core never prints: the CLI streams these to the
// terminal, the GUI to a panel.
struct VerifyEvents {
    std::function<void(const QString& chunk)> onOutput;   // live test output
    std::function<void(int attempt, int max, bool green)> onAttempt;
    std::function<void()> onFixStart;                     // handing failures to the agent
    std::function<void()> onFixStep;                      // one agent tool-turn
};

// VERIFY — makes the agent test-aware. Detects the project's test command, runs
// it, and (in the fix loop) feeds the failures back to the agent until the suite
// goes green. It is what turns "makes edits" into "makes edits that pass".
class Verify {
public:
    // `test.command` config overrides everything; otherwise we sniff the project.
    // Returns nothing when we cannot tell — guessing wrong is worse than asking.
    static std::optional<TestCommand> detect(const QString& root);

    // Runs the command line, streaming merged output to `onOutput`. Cancellation
    // terminates, then kills, OUR OWN child by handle — never a kill by name, which
    // would take out an unrelated `pytest` the user is running in another window.
    static TestRun run(const TestCommand& t, const std::function<void(const QString&)>& onOutput,
                       const CancelToken& cancel);

    // Run -> if red, ask the agent to fix -> re-run, up to `maxAttempts` rounds.
    // Returns a process exit code: 0 = green.
    static int fixLoop(const TestCommand& t, int maxAttempts, const QString& backendId,
                       const QString& model, const VerifyEvents& ev, const CancelToken& cancel);
};

}  // namespace odv
