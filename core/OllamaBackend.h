#pragma once
#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <functional>

#include "Backend.h"

namespace odv {

// The Ollama HTTP backend: /api/tags, /api/chat, /api/show, /api/ps on
// `ollama.host`.
//
// THREADING. Every public method here is called from crew worker threads, and
// several chat() calls run at once. QNetworkAccessManager is NOT thread-safe
// and is event-loop driven, so we never share one: each HTTP call constructs
// its own QNetworkAccessManager plus a local QEventLoop *on the calling
// thread*, and tears both down before returning. Nothing network-related
// outlives the call, so N std::threads may call chat() concurrently without
// any locking. The only shared state is the /api/show cache, guarded by
// cacheMutex_.
class OllamaBackend : public IModelBackend {
public:
    OllamaBackend();

    QString id() const override { return QStringLiteral("ollama"); }
    QString label() const override { return QStringLiteral("Ollama"); }

    bool available() override;
    QStringList models() override;
    QString defaultModel() override;

    // Ollama parses tool calls for us (native function calling), so the agent
    // loop gets structured calls instead of scraping prose.
    bool supportsNativeTools() const override { return true; }
    bool modelSupportsTools(const QString& model) override { return supportsTools(model); }

    ChatTurn chat(const QString& model,
                  const QVector<ChatMessage>& messages,
                  const QJsonArray& toolSchemas,
                  const StreamSink& sink,
                  const CancelToken& cancel) override;

    QJsonObject chatJson(const QString& model,
                         const QVector<ChatMessage>& messages,
                         const CancelToken& cancel) override;

    int concurrencyLimit(const QString& model) override;

    QString host() const { return host_; }

    // /api/show capability tags, e.g. ["completion","tools","thinking","vision"].
    // Cached per model — the agent loop would otherwise ask once per turn.
    QStringList capabilities(const QString& model);
    bool supportsTools(const QString& model);
    bool supportsThinking(const QString& model);
    bool supportsVision(const QString& model);

    // Trained context length from /api/show model_info ("*.context_length").
    // 0 when unknown. Cached.
    int contextLength(const QString& model);

    // /api/ps: how the loaded model is split across GPU/CPU. Empty when the
    // model is not resident.
    QJsonObject psInfo(const QString& model);

    // Ollama HTTP-400s the ENTIRE request when a tool's `properties` map is
    // serialized as an empty JSON array instead of an empty object — a schema
    // built from an empty map in a language that conflates [] and {} lands here
    // and silently kills all tool-calling. Normalising the schemas on the way
    // out is cheap insurance against that class of bug.
    static QJsonArray sanitizeTools(const QJsonArray& tools);

private:
    // Raw result of one HTTP attempt. `netError` is a QNetworkReply::NetworkError.
    struct HttpResult {
        int status = 0;   // HTTP status, 0 when the request never got that far
        int netError = 0; // QNetworkReply::NoError == 0
        QByteArray body;  // full body (also accumulated when streaming)
        QString errorText;
    };

    using ChunkFn = std::function<void(const QByteArray&)>;

    // One attempt. `onChunk` (may be null) receives bytes as they arrive, which
    // is how NDJSON streaming works. Blocking; safe on any thread.
    HttpResult request(const char* verb, const QString& path, const QByteArray& body,
                       int timeoutMs, const ChunkFn& onChunk, const CancelToken* cancel);

    // Non-streaming POST with the transient-failure retry loop.
    HttpResult postRetry(const QString& path, const QByteArray& body, int timeoutMs,
                         const CancelToken* cancel);

    // A dropped connection, a briefly-dead server, 5xx or 429 are worth retrying.
    // A clean 4xx (bad request, 401, 404) will not fix itself.
    static bool isTransient(int netError, int status);
    static void backoff(int attempt, const CancelToken* cancel);

    // Generation options for every chat request. Sets num_ctx explicitly:
    // without it Ollama caps context at 2048 tokens, which truncates the system
    // prompt and tool history out from under the agent mid-run.
    QJsonObject chatOptions(const QString& model);

    // keep_alive / think are LOCAL concerns and model-capability-gated: sending
    // think:true to a model with no thinking capability is a hard HTTP 400.
    void addModelParams(QJsonObject& body, const QString& model);

    QJsonArray encodeMessages(const QVector<ChatMessage>& messages) const;
    QJsonObject showModel(const QString& model);

    QString host_;

    QMutex cacheMutex_;
    QHash<QString, QStringList> capCache_;
    QHash<QString, int> ctxCache_;

    QMutex availMutex_;
    bool availChecked_ = false;
    bool avail_ = false;
    QElapsedTimer availAge_;
};

}  // namespace odv
