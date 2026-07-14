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

namespace odv {

// ACP — the Agent Client Protocol. JSON-RPC 2.0, newline-delimited, over stdio.
// It is how an editor (Zed, and anything else that speaks it) drives an agent that
// lives in another process: the editor owns the UI and the files, we own the model
// and the tools.
//
// The PHP `acp` command printed "ACP protocol not yet implemented." and exited.
// This is the implementation.
//
// WHAT WE SPEAK
//   client -> agent   initialize            handshake + capabilities
//                     session/new           a conversation, rooted at a cwd
//                     session/prompt        a turn; blocks until the turn ends
//                     session/cancel        (notification) stop the running turn
//   agent -> client   session/update        (notification) streamed chunks, and a
//                                           tool_call / tool_call_update per tool
//                     session/request_permission
//                                           may I run this mutating tool?
//
// THE ARCHITECTURE, AND WHY
// A turn runs on a WORKER thread while the main loop keeps reading stdin. That is
// not a detail — it is the whole thing:
//   * session/cancel arrives DURING a turn. A single-threaded loop blocked inside
//     the agent could never read it, so cancel would be impossible.
//   * session/request_permission is a request we send and whose ANSWER we must
//     read, mid-turn, on the same pipe. The worker parks on a condition variable;
//     the reader thread routes the matching response back to it.
class Acp {
public:
    // Runs until stdin closes. Returns a process exit code.
    static int serve();
};

}  // namespace odv
