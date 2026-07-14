#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

namespace odv {

// A single message in a conversation. `images` carries base64 payloads for
// vision models; `toolCallId`/`toolName` are set on role=="tool" results.
struct ChatMessage {
    QString role;  // system | user | assistant | tool
    QString content;
    QString thinking;
    QJsonArray toolCalls;  // raw, as the backend emitted them (assistant turns)
    QString toolCallId;
    QString toolName;
    QStringList images;
};

struct ToolCall {
    QString id;
    QString name;
    QJsonObject args;
};

// Result of one assistant turn.
struct ChatTurn {
    bool ok = false;
    QString content;
    QString thinking;
    QVector<ToolCall> calls;
    bool toolsUnsupported = false;  // model has no tool capability
    QString error;
    int promptTokens = 0;
    int evalTokens = 0;
};

// Streaming sink. Called from the worker thread that issued the request, so
// implementations must be thread-safe if shared. Any callback may be null.
struct StreamSink {
    std::function<void(const QString&)> onContent;
    std::function<void(const QString&)> onThinking;
};

// Cooperative cancellation. Checked between stream chunks and between tool
// iterations — never mid-write, so a cancelled agent leaves no torn files.
class CancelToken {
public:
    void cancel() { flag_->store(true); }
    bool cancelled() const { return flag_->load(); }

private:
    std::shared_ptr<std::atomic_bool> flag_ = std::make_shared<std::atomic_bool>(false);
};

// One inference provider. Every method must be safe to call from a worker
// thread; implementations own their own sockets/processes per call.
//
// This is the seam that makes "ollama and all the major CLIs" work: Ollama
// talks HTTP, everything else is a headless subprocess, and the crew does not
// care which is which.
class IModelBackend {
public:
    virtual ~IModelBackend() = default;

    virtual QString id() const = 0;     // "ollama", "claude", "codex", ...
    virtual QString label() const = 0;  // "Ollama", "Claude Code", ...

    // Is this backend usable right now (server reachable / binary on PATH)?
    virtual bool available() = 0;

    virtual QStringList models() = 0;
    virtual QString defaultModel() = 0;

    // True when the provider parses tool calls for us. Ollama does (native
    // function calling). The coding CLIs run their own agent loop instead, so
    // for those we drive them one prompt at a time and they do their own edits.
    virtual bool supportsNativeTools() const = 0;

    // Whether THIS MODEL can do tool calls, as opposed to whether the backend can.
    // The distinction is the whole story for vision: most vision models ship with
    // capabilities [completion, vision] and no `tools`, so handing them a tool
    // schema gets you an empty reply. A conversational surface should then run them
    // as a plain chat; an agent or a crew coder must fail loudly instead, because a
    // coder that cannot call edit() cannot do its job.
    //
    // Defaults to true: a backend that runs its own agent loop (the coding CLIs)
    // has no per-model notion of this.
    virtual bool modelSupportsTools(const QString& model) {
        Q_UNUSED(model);
        return true;
    }

    // Blocking single turn.
    virtual ChatTurn chat(const QString& model,
                          const QVector<ChatMessage>& messages,
                          const QJsonArray& toolSchemas,
                          const StreamSink& sink,
                          const CancelToken& cancel) = 0;

    // Blocking, non-streaming, JSON-constrained. Used by the Director,
    // Auditor and commit-message paths where we want a schema, not prose.
    virtual QJsonObject chatJson(const QString& model,
                                 const QVector<ChatMessage>& messages,
                                 const CancelToken& cancel) = 0;

    // How many calls to THIS backend may run concurrently before we are just
    // queueing inside somebody else's server. See Parallel.h for how this is
    // enforced. A local Ollama on one GPU is the constrained case; cloud models
    // and the CLI backends are not.
    virtual int concurrencyLimit(const QString& model) = 0;
};

using BackendPtr = std::shared_ptr<IModelBackend>;

// Every backend we know how to drive, whether or not it is installed.
class Backends {
public:
    // Canonical ids, in preference order.
    static QStringList all();

    // Resolve by id ("ollama", "claude", ...). Returns null for unknown ids.
    static BackendPtr get(const QString& id);

    // Only the ones actually usable on this machine.
    static QStringList availableIds();

    // Human label for an id, even if it is not installed.
    static QString labelFor(const QString& id);
};

}  // namespace odv
