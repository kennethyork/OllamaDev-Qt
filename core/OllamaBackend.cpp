#include "OllamaBackend.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopedPointer>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <utility>

#include "Config.h"
#include "Json.h"
#include "Models.h"
#include "Usage.h"

namespace odv {

namespace {

constexpr int kMaxAttempts = 3;

QString trimSlash(QString s) {
    while (s.endsWith('/')) s.chop(1);
    return s;
}

// Ollama reports a missing tool capability as a plain error string, and the
// wording has moved around across releases ("does not support tools", "model
// does not support tool calling"). Sniff both halves rather than matching one
// exact sentence.
bool looksLikeToolsUnsupported(const QString& err) {
    const QString e = err.toLower();
    return e.contains("tool") || e.contains("does not support");
}

// An optional int setting: absent/null/"" means "let Ollama decide", which is a
// different thing from 0.
bool readInt(const QJsonValue& v, int* out) {
    if (v.isDouble()) {
        *out = v.toInt();
        return true;
    }
    if (v.isString() && !v.toString().isEmpty()) {
        bool ok = false;
        const int n = v.toString().toInt(&ok);
        if (ok) *out = n;
        return ok;
    }
    return false;
}

QString errorTextOf(const QByteArray& body, int status) {
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    const QString e = o.value("error").toString();
    if (!e.isEmpty()) return e;
    if (!body.isEmpty()) return QString::fromUtf8(body).trimmed();
    return QStringLiteral("HTTP %1").arg(status);
}

}  // namespace

OllamaBackend::OllamaBackend()
    : host_(trimSlash(Config::str("ollama.host", "http://localhost:11434"))) {}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

bool OllamaBackend::isTransient(int netError, int status) {
    // Never retry a verdict: 401/403 (sign in), 404 (no such model/endpoint),
    // 400 (our request is wrong). Retrying those just burns the user's time.
    if (status == 401 || status == 403 || status == 404 || status == 400) return false;
    if (status == 500 || status == 502 || status == 503 || status == 504 || status == 429)
        return true;
    if (status != 0) return false;  // any other completed HTTP response: no retry

    switch (netError) {
        case QNetworkReply::ConnectionRefusedError:
        case QNetworkReply::RemoteHostClosedError:
        case QNetworkReply::HostNotFoundError:
        case QNetworkReply::TimeoutError:
        case QNetworkReply::TemporaryNetworkFailureError:
        case QNetworkReply::NetworkSessionFailedError:
        case QNetworkReply::ProxyConnectionClosedError:
        case QNetworkReply::ProxyTimeoutError:
        case QNetworkReply::UnknownNetworkError:
            return true;
        default:
            return false;
    }
}

// 250ms, 500ms, 1s ... capped at 2s. Sleeps in short slices so a cancel during
// the backoff is noticed immediately instead of after the full wait.
void OllamaBackend::backoff(int attempt, const CancelToken* cancel) {
    const int ms = std::min(2000, 250 * (1 << std::max(0, attempt - 1)));
    for (int slept = 0; slept < ms; slept += 50) {
        if (cancel && cancel->cancelled()) return;
        QThread::msleep(50);
    }
}

OllamaBackend::HttpResult OllamaBackend::request(const char* verb, const QString& path,
                                                 const QByteArray& body, int timeoutMs,
                                                 const ChunkFn& onChunk,
                                                 const CancelToken* cancel) {
    HttpResult r;

    // Both the manager and the event loop live on the *calling* thread and die
    // with this frame. That is what makes concurrent chat() calls from several
    // std::threads safe despite QNetworkAccessManager being thread-affine.
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(host_ + path)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    // Inactivity timeout: a long generation keeps resetting it because tokens
    // keep arriving, but a dead socket trips it.
    req.setTransferTimeout(timeoutMs);

    QEventLoop loop;
    QScopedPointer<QNetworkReply> reply(
        qstrcmp(verb, "GET") == 0 ? nam.get(req) : nam.post(req, body));

    bool cancelled = false;

    QObject::connect(reply.data(), &QNetworkReply::readyRead, &loop, [&]() {
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty()) return;
        r.body.append(chunk);
        if (onChunk) onChunk(chunk);
    });
    QObject::connect(reply.data(), &QNetworkReply::finished, &loop, &QEventLoop::quit);

    // Cancellation is cooperative and polled: CancelToken is a plain atomic with
    // no signal to connect to, and abort() must be called from this thread.
    QTimer poll;
    if (cancel) {
        QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
            if (cancel->cancelled() && reply->isRunning()) {
                cancelled = true;
                reply->abort();
            }
        });
        poll.start(100);
    }

    loop.exec();
    poll.stop();

    // abort() closes the device, and reading a closed QNetworkReply warns; the
    // bytes we already drained in readyRead are all there is anyway.
    if (reply->isOpen()) r.body.append(reply->readAll());
    r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    r.netError = static_cast<int>(reply->error());
    if (cancelled) {
        r.errorText = QStringLiteral("cancelled");
    } else if (reply->error() != QNetworkReply::NoError) {
        r.errorText = r.status != 0 ? errorTextOf(r.body, r.status) : reply->errorString();
    }
    return r;
}

OllamaBackend::HttpResult OllamaBackend::postRetry(const QString& path, const QByteArray& body,
                                                   int timeoutMs, const CancelToken* cancel) {
    HttpResult r;
    for (int attempt = 1;; ++attempt) {
        r = request("POST", path, body, timeoutMs, nullptr, cancel);
        if (cancel && cancel->cancelled()) return r;
        const bool ok = r.status == 200 && r.netError == 0;
        if (ok || attempt >= kMaxAttempts || !isTransient(r.netError, r.status)) return r;
        backoff(attempt, cancel);
    }
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

bool OllamaBackend::available() {
    QMutexLocker lock(&availMutex_);
    // Short TTL: the daemon can come up (or go down) while the GUI is open, but
    // we must not probe once per crew subtask either.
    if (availChecked_ && availAge_.isValid() && availAge_.elapsed() < 5000) return avail_;

    const HttpResult r = request("GET", "/api/tags", {}, 5000, nullptr, nullptr);
    avail_ = r.status == 200 && r.netError == 0 &&
             QJsonDocument::fromJson(r.body).object().contains("models");
    availChecked_ = true;
    availAge_.start();
    return avail_;
}

QStringList OllamaBackend::models() {
    const HttpResult r = request("GET", "/api/tags", {}, 10000, nullptr, nullptr);
    QStringList out;
    const QJsonArray arr = QJsonDocument::fromJson(r.body).object().value("models").toArray();
    for (const QJsonValue& v : arr) {
        const QString name = v.toObject().value("name").toString();
        if (!name.isEmpty()) out << name;
    }
    return out;
}

QString OllamaBackend::defaultModel() {
    const QString cfg = Config::str("ollama.defaultModel");
    if (!cfg.isEmpty()) return cfg;
    const QStringList m = models();
    return m.isEmpty() ? QStringLiteral("llama3.2:latest") : m.first();
}

QJsonObject OllamaBackend::showModel(const QString& model) {
    QJsonObject req;
    req.insert("name", model);
    const HttpResult r = request("POST", "/api/show", json::encode(req), 8000, nullptr, nullptr);
    return QJsonDocument::fromJson(r.body).object();
}

QStringList OllamaBackend::capabilities(const QString& model) {
    if (model.isEmpty()) return {};
    {
        QMutexLocker lock(&cacheMutex_);
        auto it = capCache_.constFind(model);
        if (it != capCache_.constEnd()) return it.value();
    }
    QStringList caps;
    const QJsonArray arr = showModel(model).value("capabilities").toArray();
    for (const QJsonValue& v : arr) {
        if (v.isString()) caps << v.toString();
    }
    QMutexLocker lock(&cacheMutex_);
    capCache_.insert(model, caps);
    return caps;
}

bool OllamaBackend::supportsTools(const QString& model) {
    return capabilities(model).contains(QStringLiteral("tools"));
}
bool OllamaBackend::supportsThinking(const QString& model) {
    return capabilities(model).contains(QStringLiteral("thinking"));
}
bool OllamaBackend::supportsVision(const QString& model) {
    return capabilities(model).contains(QStringLiteral("vision"));
}

int OllamaBackend::contextLength(const QString& model) {
    if (model.isEmpty()) return 0;
    {
        QMutexLocker lock(&cacheMutex_);
        auto it = ctxCache_.constFind(model);
        if (it != ctxCache_.constEnd()) return it.value();
    }
    int max = 0;
    const QJsonObject info = showModel(model).value("model_info").toObject();
    // The key is architecture-prefixed ("llama.context_length", "qwen3.context_length"),
    // so match on the suffix rather than guessing the architecture.
    for (auto it = info.constBegin(); it != info.constEnd(); ++it) {
        if (it.key().endsWith(QStringLiteral(".context_length")) && it.value().isDouble()) {
            max = it.value().toInt();
            break;
        }
    }
    QMutexLocker lock(&cacheMutex_);
    ctxCache_.insert(model, max);
    return max;
}

QJsonObject OllamaBackend::psInfo(const QString& model) {
    const HttpResult r = request("GET", "/api/ps", {}, 5000, nullptr, nullptr);
    const QJsonArray arr = QJsonDocument::fromJson(r.body).object().value("models").toArray();
    for (const QJsonValue& v : arr) {
        const QJsonObject m = v.toObject();
        if (model.isEmpty() || m.value("name").toString() == model ||
            m.value("model").toString() == model) {
            const qint64 size = static_cast<qint64>(m.value("size").toDouble());
            const qint64 vram = static_cast<qint64>(m.value("size_vram").toDouble());
            QJsonObject out;
            out.insert("name", m.value("name"));
            out.insert("size", static_cast<double>(size));
            out.insert("vram", static_cast<double>(vram));
            out.insert("gpuPct", size > 0 ? qRound(double(vram) / double(size) * 100.0) : 0);
            out.insert("context", m.value("context_length"));
            return out;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Request shaping
// ---------------------------------------------------------------------------

QJsonObject OllamaBackend::chatOptions(const QString& model) {
    const int base = Config::integer("ollama.contextWindow", 16384);
    const int cap = Config::integer("ollama.maxContextWindow", 32768);
    const bool cloud = Models::isCloud(model);

    int ctx = base;
    if (cloud) {
        // A cloud model runs on Ollama's servers, so the local VRAM cap is not a
        // constraint: give it its full trained window.
        const int max = contextLength(model);
        ctx = max > 0 ? max : Config::integer("ollama.cloudContextWindow", 131072);
    } else if (Config::boolean("ollama.autoContext", true)) {
        const int max = contextLength(model);
        const int want = max > 0 ? std::max(base, max) : base;
        ctx = std::min(want, std::max(512, cap));
    }

    const bool lowRes = Config::boolean("ollama.lowResource", false);
    if (!cloud && lowRes) {
        ctx = std::min(ctx, std::max(2048, Config::integer("ollama.lowResourceCtx", 8192)));
    }

    QJsonObject opts;
    opts.insert("num_ctx", ctx);
    opts.insert("temperature", Config::number("ollama.temperature", 0.3));

    // num_gpu = layers resident on the GPU; the rest spill to system RAM. Unset
    // means "let Ollama decide", which is right whenever the model fits in VRAM.
    // The value may arrive as a number or as a string (env / prefs are stringly
    // typed), and 0 is meaningful ("all CPU"), so presence is what gates it.
    int v = 0;
    if (readInt(Config::get("ollama.gpuLayers"), &v)) opts.insert("num_gpu", std::max(0, v));
    if (readInt(Config::get("ollama.numThreads"), &v)) opts.insert("num_thread", std::max(1, v));
    return opts;
}

void OllamaBackend::addModelParams(QJsonObject& body, const QString& model) {
    const bool cloud = Models::isCloud(model);

    // keep_alive frees local VRAM when idle. A cloud model has no local
    // footprint, so the field is meaningless there.
    if (!cloud) {
        const QString ka = Config::str("ollama.keepAlive");
        if (!ka.isEmpty()) body.insert("keep_alive", ka);
    }

    // think:true routes chain-of-thought to the `thinking` field so `content`
    // stays the clean answer. Sending it to a model without the capability is a
    // hard HTTP 400, so it is strictly capability-gated.
    if (Config::boolean("ollama.think", true) && supportsThinking(model)) {
        body.insert("think", true);
    }
}

QJsonArray OllamaBackend::encodeMessages(const QVector<ChatMessage>& messages) const {
    QJsonArray out;
    for (const ChatMessage& m : messages) {
        QJsonObject o;
        o.insert("role", m.role);
        o.insert("content", m.content);
        if (!m.thinking.isEmpty()) o.insert("thinking", m.thinking);
        if (!m.toolCalls.isEmpty()) o.insert("tool_calls", m.toolCalls);
        // Ollama matches a tool result back to its call by name; newer builds
        // also accept the id. Emitting both keeps old and new servers happy.
        if (!m.toolName.isEmpty()) o.insert("tool_name", m.toolName);
        if (!m.toolCallId.isEmpty()) o.insert("tool_call_id", m.toolCallId);
        if (!m.images.isEmpty()) {
            QJsonArray imgs;
            for (const QString& b64 : m.images) imgs.append(b64);
            o.insert("images", imgs);
        }
        out.append(o);
    }
    return out;
}

QJsonArray OllamaBackend::sanitizeTools(const QJsonArray& tools) {
    QJsonArray out;
    for (const QJsonValue& tv : tools) {
        QJsonObject tool = tv.toObject();
        QJsonObject fn = tool.value("function").toObject();
        QJsonObject params = fn.value("parameters").toObject();

        if (params.isEmpty()) params.insert("type", QStringLiteral("object"));
        if (!params.contains("type")) params.insert("type", QStringLiteral("object"));

        // THE quirk: a tool that takes no arguments must send
        // "properties": {} — an empty ARRAY there makes Ollama reject the whole
        // request with HTTP 400, killing tool-calling for every tool in the
        // batch, not just this one. QJsonObject always serializes as {}, so
        // forcing the value to an object here is the fix.
        const QJsonValue props = params.value("properties");
        params.insert("properties", props.isObject() ? props.toObject() : QJsonObject{});

        const QJsonValue req = params.value("required");
        params.insert("required", req.isArray() ? req.toArray() : QJsonArray{});

        fn.insert("parameters", params);
        tool.insert("function", fn);
        if (!tool.contains("type")) tool.insert("type", QStringLiteral("function"));
        out.append(tool);
    }
    return out;
}

// ---------------------------------------------------------------------------
// chat
// ---------------------------------------------------------------------------

ChatTurn OllamaBackend::chat(const QString& model, const QVector<ChatMessage>& messages,
                             const QJsonArray& toolSchemas, const StreamSink& sink,
                             const CancelToken& cancel) {
    ChatTurn turn;

    QJsonObject body;
    body.insert("model", model);
    body.insert("messages", encodeMessages(messages));
    body.insert("stream", true);
    body.insert("options", chatOptions(model));
    if (!toolSchemas.isEmpty()) body.insert("tools", sanitizeTools(toolSchemas));
    addModelParams(body, model);
    const QByteArray payload = json::encode(body);
    const int timeoutMs = Config::integer("ollama.timeout", 300) * 1000;

    // Parsed out of the NDJSON stream. Declared outside the retry loop only so
    // the "did anything stream yet?" check can see them.
    QString content, thinking, streamError;
    QJsonArray rawCalls;
    bool sawData = false;

    for (int attempt = 1;; ++attempt) {
        QByteArray buf;
        auto onChunk = [&](const QByteArray& chunk) {
            buf.append(chunk);
            // Each NDJSON line is a complete JSON object; a chunk may split one,
            // so only whole lines are parsed and the tail stays buffered.
            int nl;
            while ((nl = buf.indexOf('\n')) >= 0) {
                const QByteArray line = buf.left(nl).trimmed();
                buf.remove(0, nl + 1);
                if (line.isEmpty()) continue;
                const QJsonObject j = QJsonDocument::fromJson(line).object();
                if (j.isEmpty()) continue;

                // A mid-stream failure arrives as a body line, not a status code:
                // the headers said 200 long before the model refused.
                if (j.contains("error")) {
                    streamError = j.value("error").toString();
                    continue;
                }
                sawData = true;

                const QJsonObject msg = j.value("message").toObject();
                const QString cd = msg.value("content").toString();
                if (!cd.isEmpty()) {
                    content += cd;
                    if (sink.onContent) sink.onContent(cd);
                }
                const QString td = msg.value("thinking").toString();
                if (!td.isEmpty()) {
                    thinking += td;
                    if (sink.onThinking) sink.onThinking(td);
                }
                const QJsonArray calls = msg.value("tool_calls").toArray();
                for (const QJsonValue& c : calls) rawCalls.append(c);

                if (j.value("done").toBool()) {
                    turn.promptTokens = j.value("prompt_eval_count").toInt();
                    turn.evalTokens = j.value("eval_count").toInt();
                }
            }
        };

        const HttpResult r = request("POST", "/api/chat", payload, timeoutMs, onChunk, &cancel);

        if (cancel.cancelled()) {
            turn.ok = false;
            turn.error = QStringLiteral("cancelled");
            turn.content = content;
            turn.thinking = thinking;
            return turn;
        }

        const bool failed = !streamError.isEmpty() || (r.status != 200 && !sawData) ||
                            (r.netError != 0 && !sawData);

        // Retrying after tokens already reached the UI would duplicate them on
        // screen, so a partial stream is never retried — only a clean failure is.
        if (failed && !sawData && attempt < kMaxAttempts && streamError.isEmpty() &&
            isTransient(r.netError, r.status)) {
            content.clear();
            thinking.clear();
            rawCalls = QJsonArray();
            backoff(attempt, &cancel);
            continue;
        }

        if (failed) {
            QString err = streamError;
            if (err.isEmpty()) err = r.errorText;
            if (err.isEmpty()) err = QStringLiteral("HTTP %1").arg(r.status);
            turn.ok = false;
            turn.error = err;
            turn.toolsUnsupported = !toolSchemas.isEmpty() && looksLikeToolsUnsupported(err);
            return turn;
        }
        break;
    }

    turn.ok = true;
    turn.content = content;
    turn.thinking = thinking;

    int n = 0;
    for (const QJsonValue& cv : std::as_const(rawCalls)) {
        const QJsonObject c = cv.toObject();
        const QJsonObject fn = c.value("function").toObject();
        const QString name = fn.value("name").toString();
        if (name.isEmpty()) continue;

        ToolCall call;
        // Ollama usually omits the call id; the agent loop still needs a stable
        // handle to pair the result message back to this call.
        call.id = c.value("id").toString();
        if (call.id.isEmpty()) call.id = QStringLiteral("call_%1").arg(++n);
        call.name = name;

        // arguments come as an object, but some models emit them as a JSON string.
        const QJsonValue args = fn.value("arguments");
        if (args.isObject()) {
            call.args = args.toObject();
        } else if (args.isString()) {
            call.args = json::objectFrom(args.toString());
        }
        turn.calls.append(call);
    }
    return turn;
}

QJsonObject OllamaBackend::chatJson(const QString& model, const QVector<ChatMessage>& messages,
                                    const CancelToken& cancel) {
    QJsonObject body;
    body.insert("model", model);
    body.insert("messages", encodeMessages(messages));
    body.insert("stream", false);
    body.insert("format", QStringLiteral("json"));
    body.insert("options", chatOptions(model));
    // No think:true here — a plan/verdict caller wants the object, and reasoning
    // deltas would only pad the response.
    if (!Models::isCloud(model)) {
        const QString ka = Config::str("ollama.keepAlive");
        if (!ka.isEmpty()) body.insert("keep_alive", ka);
    }

    const int timeoutMs = Config::integer("ollama.timeout", 300) * 1000;
    const HttpResult r = postRetry("/api/chat", json::encode(body), timeoutMs, &cancel);
    if (r.status != 200 || r.netError != 0) return {};

    const QJsonObject root = QJsonDocument::fromJson(r.body).object();
    // Meter these too: the Director, Auditor, debate and dedupe all go through
    // chatJson, and a token report that ignored them would undercount the crew.
    Usage::record(model, root.value("prompt_eval_count").toInt(),
                  root.value("eval_count").toInt());

    const QString content =
        root.value("message").toObject().value("content").toString();
    if (content.isEmpty()) return {};

    // Even with format:json, cloud models fence their reply in ```json … ```, so
    // a plain parse returns null and the caller sees an "empty plan".
    return json::decodeLoose(content).object();
}

// ---------------------------------------------------------------------------
// Concurrency
// ---------------------------------------------------------------------------

int OllamaBackend::concurrencyLimit(const QString& model) {
    // A :cloud model is inferred on Ollama's servers — our GPU is not in the
    // loop at all, so the only ceiling is their rate limit. Fan out wide.
    if (Models::isCloud(model)) {
        return std::max(1, Config::integer("crew.cloudParallel", 8));
    }

    // LOCAL GPU. Ollama serves each loaded model from OLLAMA_NUM_PARALLEL
    // slots, and every slot costs a full num_ctx KV cache in VRAM. Issuing more
    // concurrent requests than there are slots does NOT go faster: the extras
    // just queue inside Ollama, and if the server was started with a bigger
    // OLLAMA_NUM_PARALLEL to "make room" for them, that memory is spent whether
    // the slots are busy or not. So the real slot count IS the useful limit.
    //
    // We read the env var from our own process, which is the truth when Ollama
    // runs on this machine (the normal case); when ollama.host points at a
    // remote box, crew.localParallel is the escape hatch.
    bool ok = false;
    const int env = qEnvironmentVariableIntValue("OLLAMA_NUM_PARALLEL", &ok);
    const int numSlots = (ok && env > 0) ? env : 2;  // `slots` is a Qt keyword macro
    const int cap = Config::integer("crew.localParallel", numSlots);
    return std::max(1, std::min(numSlots, cap));
}

}  // namespace odv
