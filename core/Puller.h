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

// Streaming model download via Ollama POST /api/pull (port of src/73-pull.php).
//
// A pull is a long-running download; a brief network drop must not throw away
// everything fetched so far. On a TRANSIENT failure we re-issue /api/pull and
// Ollama resumes from the layers it already has (content-addressed blobs), so it
// continues rather than restarts. A server verdict (model not found, needs auth)
// is NOT retried — that will not fix itself.
//
// CLI-only: this renders a live progress bar to stdout and owns its own SIGINT
// handler for the duration of the pull, so Ctrl-C aborts the download (by
// aborting OUR reply handle) instead of killing the process mid-write.
class Puller {
public:
    // Pulls `model` (a concrete tag, not an alias — resolve first). `host` empty
    // => ollama.host from config. Returns true on a "success" status line.
    static bool pull(const QString& model, const QString& host = {}, QString* err = nullptr);

    // Human-readable size, e.g. 1073741824 -> "1 GB".
    static QString bytes(qint64 b);
};

}  // namespace odv
