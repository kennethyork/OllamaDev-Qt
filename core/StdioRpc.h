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

namespace odv {

// Newline-delimited JSON-RPC over stdio — the transport BOTH `mcp serve` and
// `acp` speak to an editor.
//
// The hazard it exists for: on these paths stdout IS the protocol channel. One
// stray printf, one qDebug, one library warning, and the editor gets a line of
// prose where it expected a JSON-RPC frame — which does not degrade gracefully,
// it kills the session. So we MOVE the real stdout to a private descriptor and
// point fd 1 at stderr, meaning anything that carelessly writes to stdout lands
// harmlessly in the log instead of corrupting the wire.
class StdioChannel {
public:
    StdioChannel();
    ~StdioChannel();
    StdioChannel(const StdioChannel&) = delete;
    StdioChannel& operator=(const StdioChannel&) = delete;

    // The real stdout, or < 0 if it could not be captured.
    int fd() const { return saved_; }
    bool ok() const { return saved_ >= 0; }

    // One JSON-RPC frame, newline-terminated. Loops on partial writes; a hung-up
    // client is not an error here (the read loop sees EOF and stops).
    void send(const QByteArray& payload);

private:
    int saved_ = -1;
};

}  // namespace odv
