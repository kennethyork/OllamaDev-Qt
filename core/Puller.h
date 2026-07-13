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
