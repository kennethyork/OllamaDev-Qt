#include "Acp.h"

#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <atomic>
#include <iostream>
#include <memory>
#include <string>

#include "Agent.h"
#include "Backend.h"
#include "Config.h"
#include "Session.h"
#include "StdioRpc.h"
#include "Tools.h"
#include "Version.h"
#include "Vision.h"

namespace odv {
namespace {

constexpr int kProtocolVersion = 1;

// One live conversation.
struct AcpSession {
    QString id;
    QString cwd;
    QString backend;
    QString model;
    QVector<ChatMessage> messages;
    CancelToken cancel;
    std::atomic_bool running{false};
};

// The whole server's shared state. One reader thread (the stdin loop) and at most
// one worker per session, so everything crossing that line is behind `mutex`.
struct Server {
    StdioChannel out;
    QMutex mutex;

    QHash<QString, std::shared_ptr<AcpSession>> sessions;

    // Requests WE sent to the editor, waiting for their answer. The worker parks on
    // `answered` until the reader drops the response in here.
    QHash<int, QJsonObject> answers;
    QWaitCondition answered;
    int nextId = 1;

    void send(const QJsonObject& msg) {
        QMutexLocker lock(&mutex);
        out.send(QJsonDocument(msg).toJson(QJsonDocument::Compact));
    }
    void notify(const QString& method, const QJsonObject& params) {
        send(QJsonObject{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
    }
    void reply(const QJsonValue& id, const QJsonObject& result) {
        send(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
    }
    void replyError(const QJsonValue& id, int code, const QString& message) {
        send(QJsonObject{{"jsonrpc", "2.0"},
                         {"id", id},
                         {"error", QJsonObject{{"code", code}, {"message", message}}}});
    }
};

Server& server() {
    static Server s;
    return s;
}

// A request to the editor, blocking until it answers. Called from a WORKER thread
// only — the reader thread must never block on this or nothing could ever answer.
QJsonObject ask(const QString& method, const QJsonObject& params, const CancelToken& cancel) {
    Server& s = server();
    int id = 0;
    {
        QMutexLocker lock(&s.mutex);
        id = ++s.nextId;
    }
    s.send(QJsonObject{
        {"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});

    QMutexLocker lock(&s.mutex);
    while (!s.answers.contains(id)) {
        // Time-boxed so a client that never answers cannot wedge the turn forever;
        // we re-check cancellation each time round.
        s.answered.wait(&s.mutex, 250);
        if (cancel.cancelled()) return {};
    }
    return s.answers.take(id);
}

// ---------------------------------------------------------------- content blocks

// ACP content blocks -> the prompt text, plus any images onto the message.
QString promptFrom(const QJsonArray& blocks, ChatMessage* msg) {
    QStringList text;
    for (const QJsonValue& v : blocks) {
        const QJsonObject b = v.toObject();
        const QString type = b.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("text")) {
            text << b.value(QStringLiteral("text")).toString();
        } else if (type == QLatin1String("image")) {
            // Already base64 on the wire, which is exactly what Ollama wants.
            const QString data = b.value(QStringLiteral("data")).toString();
            if (!data.isEmpty() && msg) msg->images << data;
        } else if (type == QLatin1String("resource_link")) {
            const QString uri = b.value(QStringLiteral("uri")).toString();
            if (!uri.isEmpty()) text << QStringLiteral("@%1").arg(uri);
        } else if (type == QLatin1String("resource")) {
            const QJsonObject r = b.value(QStringLiteral("resource")).toObject();
            const QString t = r.value(QStringLiteral("text")).toString();
            if (!t.isEmpty())
                text << QStringLiteral("%1:\n%2")
                            .arg(r.value(QStringLiteral("uri")).toString(), t);
        }
    }
    return text.join(QLatin1Char('\n'));
}

QJsonObject textUpdate(const QString& kind, const QString& text) {
    return QJsonObject{{"sessionUpdate", kind},
                       {"content", QJsonObject{{"type", "text"}, {"text", text}}}};
}

// ------------------------------------------------------------------- the turn

void runTurn(const std::shared_ptr<AcpSession>& sess, const QJsonValue& reqId,
             const QJsonArray& promptBlocks) {
    Server& s = server();

    ChatMessage user;
    user.role = QStringLiteral("user");
    const QString raw = promptFrom(promptBlocks, &user);
    // @file / @image tokens work here exactly as they do everywhere else.
    user.content = Vision::attach(user, raw);
    sess->messages.append(user);

    Agent agent(sess->backend, sess->model);
    Tools::setThreadRoot(sess->cwd);

    // Every mutating tool becomes a question the EDITOR asks its user. This is the
    // point of ACP: the permission UI belongs to the editor, not to us.
    Permission::setMode(PermMode::Ask);
    Permission::setInteractive(true);
    Permission::setAsker([&](const ToolDef& t, const QJsonObject& args) {
        const QJsonObject res = ask(
            QStringLiteral("session/request_permission"),
            QJsonObject{
                {"sessionId", sess->id},
                {"toolCall",
                 QJsonObject{{"toolCallId", t.name},
                             {"title", t.name},
                             {"kind", t.mutates ? "edit" : "read"},
                             {"rawInput", args}}},
                {"options",
                 QJsonArray{
                     QJsonObject{{"optionId", "allow"}, {"name", "Allow"}, {"kind", "allow_once"}},
                     QJsonObject{
                         {"optionId", "always"}, {"name", "Always allow"}, {"kind", "allow_always"}},
                     QJsonObject{{"optionId", "reject"}, {"name", "Reject"}, {"kind", "reject_once"}}}}},
            sess->cancel);

        const QJsonObject outcome =
            res.value(QStringLiteral("result")).toObject().value(QStringLiteral("outcome")).toObject();
        const QString picked = outcome.value(QStringLiteral("optionId")).toString();
        if (picked == QLatin1String("always")) {
            Permission::setMode(PermMode::Auto);  // for the rest of this turn
            return true;
        }
        return picked == QLatin1String("allow");
    });

    StreamSink sink;
    sink.onContent = [&](const QString& chunk) {
        s.notify(QStringLiteral("session/update"),
                 QJsonObject{{"sessionId", sess->id},
                             {"update", textUpdate(QStringLiteral("agent_message_chunk"), chunk)}});
    };
    sink.onThinking = [&](const QString& chunk) {
        s.notify(QStringLiteral("session/update"),
                 QJsonObject{{"sessionId", sess->id},
                             {"update", textUpdate(QStringLiteral("agent_thought_chunk"), chunk)}});
    };
    // The editor draws a tool card from this. Sent BEFORE the tool runs, so the
    // user watches it happen rather than being told afterwards.
    sink.onTool = [&](const QString& tool, const QString& detail) {
        s.notify(QStringLiteral("session/update"),
                 QJsonObject{{"sessionId", sess->id},
                             {"update", QJsonObject{{"sessionUpdate", "tool_call"},
                                                    {"toolCallId", tool},
                                                    {"title", detail.isEmpty()
                                                                  ? tool
                                                                  : QStringLiteral("%1 %2")
                                                                        .arg(tool, detail)},
                                                    {"status", "in_progress"}}}});
    };

    const QString finalText =
        agent.loop(sess->messages, Config::integer(QStringLiteral("agents.maxIterations"), 20), sink,
                   sess->cancel);
    Q_UNUSED(finalText);

    // Persist, so `session/load` and the CLI's own `-c` can pick this conversation
    // up later. An ACP turn is a real session, not a scratchpad.
    Session store = Session::create(sess->cwd);
    store.messages() = sess->messages;
    store.setModel(sess->model);
    store.setBackend(sess->backend);
    store.save();

    sess->running = false;
    s.reply(reqId, QJsonObject{{"stopReason", sess->cancel.cancelled() ? "cancelled" : "end_turn"}});
}

// A turn on its own thread. QThread rather than std::thread so it composes with
// Qt's event machinery if this ever needs to.
class TurnThread : public QThread {
public:
    TurnThread(std::shared_ptr<AcpSession> s, QJsonValue id, QJsonArray prompt)
        : sess_(std::move(s)), id_(std::move(id)), prompt_(std::move(prompt)) {
        setObjectName(QStringLiteral("acp-turn"));
        connect(this, &QThread::finished, this, &QObject::deleteLater);
    }

protected:
    void run() override { runTurn(sess_, id_, prompt_); }

private:
    std::shared_ptr<AcpSession> sess_;
    QJsonValue id_;
    QJsonArray prompt_;
};

// ------------------------------------------------------------------- dispatch

void handle(const QJsonObject& msg) {
    Server& s = server();
    const QJsonValue id = msg.value(QStringLiteral("id"));
    const QString method = msg.value(QStringLiteral("method")).toString();
    const QJsonObject params = msg.value(QStringLiteral("params")).toObject();

    // A RESPONSE to something we asked (it has an id and no method). Hand it to the
    // worker that is parked waiting for it.
    if (method.isEmpty() && !id.isUndefined()) {
        QMutexLocker lock(&s.mutex);
        s.answers.insert(id.toInt(), msg);
        s.answered.wakeAll();
        return;
    }

    if (method == QLatin1String("initialize")) {
        s.reply(id,
                QJsonObject{
                    {"protocolVersion", kProtocolVersion},
                    {"agentCapabilities",
                     QJsonObject{{"loadSession", false},
                                 {"promptCapabilities",
                                  QJsonObject{{"image", true}, {"embeddedContext", true}}}}},
                    // No auth: we talk to a local Ollama. Saying so explicitly is
                    // better than leaving the editor to guess.
                    {"authMethods", QJsonArray{}},
                    {"agentInfo",
                     QJsonObject{{"name", "ollamadev"}, {"version", QStringLiteral(ODV_VERSION)}}}});
        return;
    }

    if (method == QLatin1String("authenticate")) {
        s.reply(id, QJsonObject{});  // nothing to authenticate against
        return;
    }

    if (method == QLatin1String("session/new")) {
        auto sess = std::make_shared<AcpSession>();
        sess->id = QStringLiteral("acp_%1").arg(++s.nextId);
        sess->cwd = params.value(QStringLiteral("cwd")).toString(QDir::currentPath());
        sess->backend = Config::str(QStringLiteral("model.backend"), QStringLiteral("ollama"));
        sess->model = Config::str(QStringLiteral("ollama.defaultModel"), QString());
        if (sess->model.isEmpty()) {
            if (auto b = Backends::get(sess->backend)) sess->model = b->defaultModel();
        }
        {
            QMutexLocker lock(&s.mutex);
            s.sessions.insert(sess->id, sess);
        }
        s.reply(id, QJsonObject{{"sessionId", sess->id}});
        return;
    }

    if (method == QLatin1String("session/prompt")) {
        const QString sid = params.value(QStringLiteral("sessionId")).toString();
        std::shared_ptr<AcpSession> sess;
        {
            QMutexLocker lock(&s.mutex);
            sess = s.sessions.value(sid);
        }
        if (!sess) {
            s.replyError(id, -32602, QStringLiteral("no such session"));
            return;
        }
        if (sess->running.exchange(true)) {
            s.replyError(id, -32603, QStringLiteral("that session is already in a turn"));
            return;
        }
        sess->cancel = CancelToken();  // a fresh token per turn

        // On a worker, so the reader loop below stays free to take session/cancel
        // and to route the answer to a permission request. This is the reason the
        // whole server is threaded.
        auto* t = new TurnThread(sess, id, params.value(QStringLiteral("prompt")).toArray());
        t->start();
        return;
    }

    if (method == QLatin1String("session/cancel")) {
        const QString sid = params.value(QStringLiteral("sessionId")).toString();
        QMutexLocker lock(&s.mutex);
        if (auto sess = s.sessions.value(sid)) {
            sess->cancel.cancel();
            s.answered.wakeAll();  // unpark a worker sitting on a permission request
        }
        return;  // a notification: no reply
    }

    if (!id.isUndefined()) s.replyError(id, -32601, QStringLiteral("no method '%1'").arg(method));
}

}  // namespace

int Acp::serve() {
    Tools::registerAll();
    Server& s = server();
    if (!s.out.ok()) return 1;

    // Newline-delimited JSON-RPC on stdin. stdout is already jailed by StdioChannel
    // — anything that carelessly prints lands on stderr instead of corrupting the
    // protocol, which does not degrade gracefully: one stray byte and the editor's
    // parser desynchronises for the rest of the session.
    std::string line;
    while (std::getline(std::cin, line)) {
        const QByteArray raw = QByteArray::fromStdString(line).trimmed();
        if (raw.isEmpty()) continue;
        const QJsonObject msg = QJsonDocument::fromJson(raw).object();
        if (msg.isEmpty()) continue;
        handle(msg);
    }

    // stdin closed: the editor is gone. Stop any turn still in flight rather than
    // leaving a model call running against a socket nobody is reading.
    QMutexLocker lock(&s.mutex);
    for (auto& sess : s.sessions)
        if (sess) sess->cancel.cancel();
    return 0;
}

}  // namespace odv
