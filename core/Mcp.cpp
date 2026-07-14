// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Mcp.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopedPointer>
#include <QThread>
#include <QUrl>
#include <functional>
#include <iostream>
#include <string>

#include "Config.h"
#include "Plugins.h"
#include "Json.h"
#include "Version.h"

#ifdef Q_OS_WIN
#include <io.h>
#define ODV_DUP _dup
#define ODV_DUP2 _dup2
#define ODV_WRITE _write
#define ODV_CLOSE _close
#define ODV_FD_OUT 1
#define ODV_FD_ERR 2
#else
#include <unistd.h>
#define ODV_DUP ::dup
#define ODV_DUP2 ::dup2
#define ODV_WRITE ::write
#define ODV_CLOSE ::close
#define ODV_FD_OUT STDOUT_FILENO
#define ODV_FD_ERR STDERR_FILENO
#endif

namespace odv {
namespace {

constexpr const char* kProtocolVersion = "2024-11-05";
constexpr int kRpcTimeoutMs = 30000;

// ------------------------------------------------------------------ config i/o

// Config::load() takes the first file that exists, and Config never writes
// config.json (setPref writes ade-prefs.json). MCP config lives in config.json by
// convention, so we write it ourselves — into the SAME file the loader will read,
// or the primary path if none exists yet. Writing blindly to ~/.ollamadev/config.json
// would shadow a config at ~/.config/ollamadev and silently orphan the user's file.
QString configPath() {
    const QString home = QDir::homePath();
    const QStringList paths{
        home + QStringLiteral("/.ollamadev/config.json"),
        home + QStringLiteral("/.config/ollamadev/config.json"),
        QDir::current().filePath(QStringLiteral(".ollamadev.json")),
    };
    for (const QString& p : paths)
        if (QFileInfo::exists(p)) return p;
    return paths.first();
}

QJsonObject readConfigFile() {
    QFile f(configPath());
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray raw = f.readAll();
    f.close();
    return json::objectFrom(QString::fromUtf8(raw));
}

bool writeConfigFile(const QJsonObject& o, QString* err) {
    const QString path = configPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) *err = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    out.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    if (!out.commit()) {
        if (err) *err = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    return true;
}

// A tool name the model can actually emit: function names are restricted to
// [A-Za-z0-9_-] by the providers, so "server/tool" (the PHP's internal key) is
// not a legal wire name.
QString toolNameFor(const QString& server, const QString& tool) {
    QString s = QStringLiteral("mcp_%1_%2").arg(server, tool);
    for (QChar& c : s) {
        if (!c.isLetterOrNumber() && c != QLatin1Char('_') && c != QLatin1Char('-'))
            c = QLatin1Char('_');
    }
    return s;
}

// MCP tool results are a content[] of typed parts; we want the text.
QString extractText(const QJsonValue& result) {
    if (result.isString()) return result.toString();
    const QJsonArray content = result.toObject().value(QStringLiteral("content")).toArray();
    QStringList texts;
    for (const QJsonValue& c : content) {
        const QString t = c.toObject().value(QStringLiteral("text")).toString();
        if (!t.isEmpty()) texts << t;
    }
    if (!texts.isEmpty()) return texts.join(QLatin1Char('\n'));
    if (result.isObject()) return QString::fromUtf8(json::encode(result.toObject()));
    return {};
}

// A schema Ollama will accept: `properties` must be an OBJECT, never an array —
// see the QUIRK note in Tools.h. A remote server can send us anything.
QJsonObject sanitizeSchema(const QJsonValue& v) {
    QJsonObject s = v.toObject();
    if (!s.contains(QStringLiteral("type"))) s.insert(QStringLiteral("type"), QStringLiteral("object"));
    const QJsonValue props = s.value(QStringLiteral("properties"));
    s.insert(QStringLiteral("properties"), props.isObject() ? props.toObject() : QJsonObject{});
    const QJsonValue req = s.value(QStringLiteral("required"));
    s.insert(QStringLiteral("required"), req.isArray() ? req.toArray() : QJsonArray{});
    return s;
}

// ------------------------------------------------------------------ stdout jail

// On the `mcp serve` path stdout IS the JSON-RPC channel: one stray byte and the
// client's parser desynchronises for the rest of the session. Our tools return
// output through ToolResult rather than printing, but a tool can shell out, and a
// future one may not be so careful — so fd 1 is pointed at fd 2 for the whole
// serve loop and the real channel is kept as a private descriptor that only the
// RPC writer holds. Anything that prints "to stdout" from here on — us, a Qt
// warning, a child process that inherits our descriptors — lands on stderr, where
// it is visible to the user and harmless to the protocol.
//
// PHP used ob_start(); C++ has no ambient output buffer, and an output buffer
// would not have caught a child process anyway.
class StdoutJail {
public:
    StdoutJail() {
        saved_ = ODV_DUP(ODV_FD_OUT);
        if (saved_ < 0) return;
        // Deliberately NOT fflush(stdout) first: anything already sitting in the
        // stdio buffer would be flushed onto the real fd 1 — corrupting the very
        // channel we are protecting. Leaving it buffered means it goes to stderr
        // at the flush below instead.
        ODV_DUP2(ODV_FD_ERR, ODV_FD_OUT);
    }
    ~StdoutJail() {
        if (saved_ < 0) return;
        fflush(stdout);  // drains to fd 1, which is still stderr
        ODV_DUP2(saved_, ODV_FD_OUT);
        ODV_CLOSE(saved_);
    }
    StdoutJail(const StdoutJail&) = delete;
    StdoutJail& operator=(const StdoutJail&) = delete;

    int channel() const { return saved_; }  // the real stdout

private:
    int saved_ = -1;
};

void writeLine(int fd, const QByteArray& payload) {
    QByteArray buf = payload;
    buf.append('\n');
    qint64 off = 0;
    while (off < buf.size()) {
        const auto n = ODV_WRITE(fd, buf.constData() + off, static_cast<unsigned>(buf.size() - off));
        if (n <= 0) return;  // the client hung up; the read loop will see EOF too
        off += n;
    }
}

// ----------------------------------------------------------- client keep-alive

// Discovered clients outlive discoverTools() because the ToolDefs they produced
// hold them. Deliberately leaked rather than a plain static: at static-destruction
// time QCoreApplication is already gone and tearing down a QThread there is a
// crash. qAddPostRoutine shuts the children down while Qt is still alive.
QVector<std::shared_ptr<McpClient>>& liveClients() {
    static auto* v = new QVector<std::shared_ptr<McpClient>>();
    return *v;
}

void shutdownClients() {
    for (auto& c : liveClients())
        if (c) c->shutdown();
    liveClients().clear();
}

}  // namespace

// ============================================================== McpClient

struct McpClient::Impl {
    QThread thread;
    QObject* ctx = nullptr;    // lives in `thread`; the context for every call
    QProcess* proc = nullptr;  // created in, and owned by, `thread`
    QByteArray rx;             // bytes read past the end of the last message
    int rpcId = 0;
    bool initialized = false;
    bool failed = false;
};

McpClient::McpClient(McpServerCfg cfg) : d_(std::make_unique<Impl>()), cfg_(std::move(cfg)) {
    if (cfg_.type == QLatin1String("stdio")) {
        d_->ctx = new QObject();
        d_->ctx->moveToThread(&d_->thread);
        d_->thread.start();
    }
}

McpClient::~McpClient() {
    shutdown();
    if (d_->ctx) {
        d_->ctx->deleteLater();
        d_->ctx = nullptr;
    }
    if (d_->thread.isRunning()) {
        d_->thread.quit();
        d_->thread.wait(5000);
    }
}

void McpClient::shutdown() {
    if (!d_->thread.isRunning()) return;
    // Terminate OUR child, by handle. Never a name-based kill: a pkill would take
    // out an identical server owned by another OllamaDev instance.
    QMetaObject::invokeMethod(
        d_->ctx,
        [this] {
            if (!d_->proc) return;
            if (d_->proc->state() != QProcess::NotRunning) {
                d_->proc->closeWriteChannel();  // a well-behaved server exits on EOF
                if (!d_->proc->waitForFinished(2000)) {
                    d_->proc->terminate();
                    if (!d_->proc->waitForFinished(2000)) d_->proc->kill();
                }
            }
            delete d_->proc;
            d_->proc = nullptr;
        },
        Qt::BlockingQueuedConnection);
}

namespace {

// Both LSP framings in the wild: the spec says \r\n\r\n, some servers emit \n\n.
int headerEnd(const QByteArray& buf, int* sepLen) {
    const int crlf = buf.indexOf("\r\n\r\n");
    const int lf = buf.indexOf("\n\n");
    if (crlf >= 0 && (lf < 0 || crlf <= lf)) {
        *sepLen = 4;
        return crlf;
    }
    if (lf >= 0) {
        *sepLen = 2;
        return lf;
    }
    return -1;
}

}  // namespace

// Everything below with a `proc` argument runs INSIDE the client's thread.
namespace {

bool procStart(McpServerCfg& cfg, QProcess*& proc, bool& failed) {
    if (proc && proc->state() == QProcess::Running) return true;
    if (failed || cfg.command.isEmpty()) return false;

    proc = new QProcess();
    // The child's stderr goes to OUR stderr. It must NEVER be ForwardedChannels:
    // that would also splice the child's stdout — which is the framed JSON-RPC we
    // are about to parse — onto our own stdout and wreck `mcp serve`'s channel.
    proc->setProcessChannelMode(QProcess::ForwardedErrorChannel);

    if (!cfg.env.isEmpty()) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (auto it = cfg.env.constBegin(); it != cfg.env.constEnd(); ++it)
            env.insert(it.key(), it.value().toString());
        proc->setProcessEnvironment(env);
    }

    // program + argv, never a shell string. The PHP glued the command together
    // with escapeshellarg and handed it to a shell; QProcess passes argv straight
    // to exec, so an argument with a space or a quote cannot become a new word.
    proc->start(cfg.command, cfg.args);
    if (!proc->waitForStarted(10000)) {
        failed = true;
        delete proc;
        proc = nullptr;
        return false;
    }
    return true;
}

void procWrite(QProcess* proc, const QJsonObject& msg) {
    const QByteArray body = QJsonDocument(msg).toJson(QJsonDocument::Compact);
    proc->write("Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n");
    proc->write(body);
    proc->waitForBytesWritten(5000);
}

QJsonObject procRead(QProcess* proc, QByteArray& rx, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();

    int sepLen = 0;
    int sep = headerEnd(rx, &sepLen);
    while (sep < 0) {
        if (clock.elapsed() > timeoutMs || proc->state() == QProcess::NotRunning) return {};
        if (proc->waitForReadyRead(200)) rx.append(proc->readAllStandardOutput());
        sep = headerEnd(rx, &sepLen);
    }

    static const QRegularExpression len(QStringLiteral("Content-Length:\\s*(\\d+)"),
                                        QRegularExpression::CaseInsensitiveOption);
    const auto m = len.match(QString::fromUtf8(rx.left(sep)));
    if (!m.hasMatch()) {
        rx.remove(0, sep + sepLen);  // unframed junk — drop it rather than spin
        return {};
    }
    const int want = m.captured(1).toInt();
    rx.remove(0, sep + sepLen);

    while (rx.size() < want) {
        if (clock.elapsed() > timeoutMs || proc->state() == QProcess::NotRunning) return {};
        if (proc->waitForReadyRead(200)) rx.append(proc->readAllStandardOutput());
    }
    const QByteArray body = rx.left(want);
    rx.remove(0, want);
    return QJsonDocument::fromJson(body).object();
}

}  // namespace

// HTTP transport. No process and no thread: a frame-local QNetworkAccessManager is
// safe on any thread, so these calls just run on the caller's.
//
// One JSON-RPC POST to the configured URL — the streamable-http transport. (The
// PHP invented its own /tools and /rpc sub-paths, which no MCP server serves.)
namespace {

QJsonObject httpRpc(const McpServerCfg& cfg, const QString& method, const QJsonObject& params) {
    QNetworkRequest req{QUrl(cfg.url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", "application/json, text/event-stream");
    req.setTransferTimeout(kRpcTimeoutMs);
    for (auto it = cfg.headers.constBegin(); it != cfg.headers.constEnd(); ++it)
        req.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());

    QJsonObject msg{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}};
    if (!params.isEmpty()) msg.insert(QStringLiteral("params"), params);

    QNetworkAccessManager nam;
    QEventLoop loop;
    QScopedPointer<QNetworkReply> reply(nam.post(req, json::encode(msg)));
    QObject::connect(reply.data(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (reply->error() != QNetworkReply::NoError) return {};

    // An SSE-framed reply carries the JSON on a `data:` line.
    const QByteArray raw = reply->readAll();
    QJsonObject o = QJsonDocument::fromJson(raw).object();
    if (o.isEmpty()) {
        for (const QByteArray& line : raw.split('\n')) {
            if (!line.startsWith("data:")) continue;
            o = QJsonDocument::fromJson(line.mid(5).trimmed()).object();
            if (!o.isEmpty()) break;
        }
    }
    return o;
}

}  // namespace

QJsonArray McpClient::listTools() {
    if (cfg_.type != QLatin1String("stdio")) {
        if (cfg_.url.isEmpty()) return {};
        return httpRpc(cfg_, QStringLiteral("tools/list"), {})
            .value(QStringLiteral("result"))
            .toObject()
            .value(QStringLiteral("tools"))
            .toArray();
    }

    QJsonArray tools;
    QMetaObject::invokeMethod(
        d_->ctx,
        [this, &tools] {
            if (!procStart(cfg_, d_->proc, d_->failed)) return;

            if (!d_->initialized) {
                QJsonObject init{{"protocolVersion", QLatin1String(kProtocolVersion)},
                                 {"capabilities", QJsonObject{{"tools", QJsonObject{}}}},
                                 {"clientInfo", QJsonObject{{"name", "ollamadev"},
                                                            {"version", QLatin1String(ODV_VERSION)}}}};
                procWrite(d_->proc, QJsonObject{{"jsonrpc", "2.0"},
                                                {"id", ++d_->rpcId},
                                                {"method", "initialize"},
                                                {"params", init}});
                const QJsonObject resp = procRead(d_->proc, d_->rx, kRpcTimeoutMs);
                if (resp.isEmpty()) {
                    d_->failed = true;
                    return;
                }
                // Fire-and-forget: the handshake is only complete once the server
                // has been told we are ready.
                procWrite(d_->proc,
                          QJsonObject{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
                d_->initialized = true;
            }

            const int id = ++d_->rpcId;
            procWrite(d_->proc,
                      QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/list"}});
            // Skip any server-initiated notification/log that arrives first.
            for (int i = 0; i < 20; ++i) {
                const QJsonObject resp = procRead(d_->proc, d_->rx, kRpcTimeoutMs);
                if (resp.isEmpty()) return;
                if (resp.value(QStringLiteral("id")).toInt(-1) != id) continue;
                tools = resp.value(QStringLiteral("result"))
                            .toObject()
                            .value(QStringLiteral("tools"))
                            .toArray();
                return;
            }
        },
        Qt::BlockingQueuedConnection);
    return tools;
}

QString McpClient::callTool(const QString& name, const QJsonObject& args, bool* ok) {
    if (ok) *ok = false;

    if (cfg_.type != QLatin1String("stdio")) {
        if (cfg_.url.isEmpty()) return QStringLiteral("MCP: server '%1' has no url").arg(cfg_.name);
        const QJsonObject resp =
            httpRpc(cfg_, QStringLiteral("tools/call"),
                    QJsonObject{{"name", name}, {"arguments", args}});
        if (resp.isEmpty()) return QStringLiteral("MCP: no response from %1").arg(cfg_.url);
        if (resp.contains(QStringLiteral("error")))
            return QStringLiteral("MCP error: %1")
                .arg(resp.value(QStringLiteral("error")).toObject().value("message").toString());
        if (ok) *ok = true;
        return extractText(resp.value(QStringLiteral("result")));
    }

    QString text;
    bool good = false;
    QMetaObject::invokeMethod(
        d_->ctx,
        [&] {
            if (!d_->initialized) {
                // listTools() performs the handshake; a cold call must too.
                if (!procStart(cfg_, d_->proc, d_->failed)) {
                    text = QStringLiteral("MCP: failed to start server '%1'").arg(cfg_.command);
                    return;
                }
            }
            if (d_->failed || !d_->proc) {
                text = QStringLiteral("MCP: server '%1' is not running").arg(cfg_.name);
                return;
            }

            const int id = ++d_->rpcId;
            procWrite(d_->proc, QJsonObject{{"jsonrpc", "2.0"},
                                            {"id", id},
                                            {"method", "tools/call"},
                                            {"params", QJsonObject{{"name", name},
                                                                   {"arguments", args}}}});
            for (int i = 0; i < 20; ++i) {
                const QJsonObject resp = procRead(d_->proc, d_->rx, kRpcTimeoutMs);
                if (resp.isEmpty()) {
                    text = QStringLiteral("MCP: no response from server");
                    return;
                }
                if (resp.value(QStringLiteral("id")).toInt(-1) != id) continue;
                if (resp.contains(QStringLiteral("error"))) {
                    text = QStringLiteral("MCP error: %1")
                               .arg(resp.value(QStringLiteral("error"))
                                        .toObject()
                                        .value(QStringLiteral("message"))
                                        .toString());
                    return;
                }
                text = extractText(resp.value(QStringLiteral("result")));
                good = true;
                return;
            }
            text = QStringLiteral("MCP: no matching response from server");
        },
        Qt::BlockingQueuedConnection);

    if (ok) *ok = good;
    return text;
}

// ==================================================================== Mcp

QVector<McpServerCfg> Mcp::servers() {
    QJsonObject obj = Config::get(QStringLiteral("mcpServers")).toObject();

    // Adopt servers written by the PHP CLI's `mcp add`, which stored them under
    // `mcp` — a key its own loader never read. Only entries that look like a
    // server are taken, so the scalar `mcp.allowWrites` is left alone.
    const QJsonObject legacy = Config::get(QStringLiteral("mcp")).toObject();
    for (auto it = legacy.constBegin(); it != legacy.constEnd(); ++it) {
        if (obj.contains(it.key()) || !it.value().isObject()) continue;
        const QJsonObject e = it.value().toObject();
        if (!e.contains(QStringLiteral("command")) && !e.contains(QStringLiteral("url"))) continue;
        obj.insert(it.key(), e);
    }

    // Servers contributed by an enabled plugin. A configured server of the same
    // name wins — your own config is never overridden by something you installed.
    const QJsonObject fromPlugins = Plugins::mcpServers();
    for (auto it = fromPlugins.constBegin(); it != fromPlugins.constEnd(); ++it)
        if (!obj.contains(it.key()) && it.value().isObject()) obj.insert(it.key(), it.value());

    QVector<McpServerCfg> out;
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        const QJsonObject e = it.value().toObject();
        McpServerCfg c;
        c.name = it.key();
        c.command = e.value(QStringLiteral("command")).toString();
        c.url = e.value(QStringLiteral("url")).toString();
        c.type = e.value(QStringLiteral("type")).toString();
        if (c.type.isEmpty()) c.type = c.command.isEmpty() ? QStringLiteral("http") : QStringLiteral("stdio");
        for (const QJsonValue& a : e.value(QStringLiteral("args")).toArray()) c.args << a.toString();
        c.headers = e.value(QStringLiteral("headers")).toObject();
        c.env = e.value(QStringLiteral("env")).toObject();
        // `disabled: true` and `enabled: false` both mean off — the PHP wrote the
        // latter and read the former.
        c.disabled = e.value(QStringLiteral("disabled")).toBool(false) ||
                     (e.contains(QStringLiteral("enabled")) &&
                      !e.value(QStringLiteral("enabled")).toBool(true));
        out.append(c);
    }
    return out;
}

bool Mcp::addServer(const QString& name, const QString& command, const QStringList& args,
                    QString* err) {
    if (name.isEmpty() || command.isEmpty()) {
        if (err) *err = QStringLiteral("usage: ollamadev mcp add <name> <command> [args...]");
        return false;
    }
    QJsonObject cfg = readConfigFile();
    QJsonObject servers = cfg.value(QStringLiteral("mcpServers")).toObject();

    QJsonObject entry{{"command", command}, {"type", "stdio"}};
    if (!args.isEmpty()) {
        QJsonArray a;
        for (const QString& s : args) a.append(s);
        entry.insert(QStringLiteral("args"), a);
    }
    servers.insert(name, entry);
    cfg.insert(QStringLiteral("mcpServers"), servers);
    return writeConfigFile(cfg, err);
}

bool Mcp::removeServer(const QString& name, QString* err) {
    QJsonObject cfg = readConfigFile();
    QJsonObject servers = cfg.value(QStringLiteral("mcpServers")).toObject();
    QJsonObject legacy = cfg.value(QStringLiteral("mcp")).toObject();

    const bool inNew = servers.contains(name);
    const bool inOld = legacy.contains(name) && legacy.value(name).isObject();
    if (!inNew && !inOld) {
        if (err) *err = QStringLiteral("server not found: %1").arg(name);
        return false;
    }
    servers.remove(name);
    cfg.insert(QStringLiteral("mcpServers"), servers);
    if (inOld) {  // also clear the legacy copy, or it would be adopted again
        legacy.remove(name);
        cfg.insert(QStringLiteral("mcp"), legacy);
    }
    return writeConfigFile(cfg, err);
}

QVector<ToolDef> Mcp::discoverTools() {
    const QVector<McpServerCfg> cfgs = servers();
    if (cfgs.isEmpty()) return {};  // the common case: no servers, nothing spawned

    static bool routineInstalled = false;
    if (!routineInstalled) {
        qAddPostRoutine(shutdownClients);
        routineInstalled = true;
    }

    QVector<ToolDef> defs;
    for (const McpServerCfg& cfg : cfgs) {
        if (cfg.disabled) continue;

        auto client = std::make_shared<McpClient>(cfg);
        const QJsonArray tools = client->listTools();
        if (tools.isEmpty()) {
            qWarning("mcp: server '%s' reported no tools (not started?)", qPrintable(cfg.name));
            continue;
        }
        liveClients().append(client);  // the ToolDefs below borrow it

        for (const QJsonValue& tv : tools) {
            const QJsonObject t = tv.toObject();
            const QString remote = t.value(QStringLiteral("name")).toString();
            if (remote.isEmpty()) continue;

            ToolDef d;
            d.name = toolNameFor(cfg.name, remote);
            d.description = QStringLiteral("[mcp:%1] %2")
                                .arg(cfg.name, t.value(QStringLiteral("description")).toString());
            d.parameters = sanitizeSchema(t.value(QStringLiteral("inputSchema")));

            // We cannot see what a remote tool does, so it is assumed to mutate
            // unless the server explicitly says otherwise. Guessing the other way
            // would hand a read-only session a write tool.
            const QJsonObject ann = t.value(QStringLiteral("annotations")).toObject();
            d.mutates = !ann.value(QStringLiteral("readOnlyHint")).toBool(false);

            d.fn = [client, remote](const QJsonObject& args) -> ToolResult {
                bool ok = false;
                const QString text = client->callTool(remote, args, &ok);
                if (!ok) return ToolResult{false, QString(), text};
                return ToolResult{true, text, QString()};
            };
            defs.append(d);
        }
    }
    return defs;
}

// ============================================================== McpServer

namespace {

QJsonObject rpcOk(const QJsonValue& id, const QJsonValue& result) {
    return QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

QJsonObject rpcErr(const QJsonValue& id, int code, const QString& message) {
    return QJsonObject{{"jsonrpc", "2.0"},
                       {"id", id},
                       {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

// Our native function-call schemas → MCP tool descriptors.
QJsonArray serverToolList() {
    QJsonArray out;
    for (const QJsonValue& sv : Tools::schemas()) {
        const QJsonObject f = sv.toObject().value(QStringLiteral("function")).toObject();
        const QString name = f.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) continue;
        out.append(QJsonObject{
            {"name", name},
            {"description", f.value(QStringLiteral("description")).toString()},
            {"inputSchema", sanitizeSchema(f.value(QStringLiteral("parameters")))}});
    }
    return out;
}

// Returns false when the message needs no reply (a notification).
bool handleRpc(const QJsonObject& msg, QJsonObject* out) {
    const QJsonValue id = msg.value(QStringLiteral("id"));
    const QString method = msg.value(QStringLiteral("method")).toString();
    const bool isNotification = id.isUndefined() || id.isNull();

    if (isNotification && method.startsWith(QLatin1String("notifications/"))) return false;

    if (method == QLatin1String("initialize")) {
        *out = rpcOk(id, QJsonObject{{"protocolVersion", QLatin1String(kProtocolVersion)},
                                     {"capabilities", QJsonObject{{"tools", QJsonObject{}}}},
                                     {"serverInfo", QJsonObject{{"name", "ollamadev"},
                                                                {"version", QLatin1String(ODV_VERSION)}}}});
        return true;
    }
    if (method == QLatin1String("ping")) {
        *out = rpcOk(id, QJsonObject{});
        return true;
    }
    if (method == QLatin1String("tools/list")) {
        *out = rpcOk(id, QJsonObject{{"tools", serverToolList()}});
        return true;
    }
    if (method == QLatin1String("tools/call")) {
        const QJsonObject params = msg.value(QStringLiteral("params")).toObject();
        const QString name = params.value(QStringLiteral("name")).toString();
        const QJsonObject args = params.value(QStringLiteral("arguments")).toObject();
        if (!Tools::find(name)) {
            *out = rpcErr(id, -32602, QStringLiteral("Unknown tool: %1").arg(name));
            return true;
        }

        const ToolResult r = Tools::run(name, args);
        const QString text = r.ok ? r.output : r.error;
        // isError:true lets a client tell a blocked or failed tool (permission
        // denied in read-only mode, bad arguments) from a real answer.
        *out = rpcOk(id, QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"},
                                                                        {"text", text}}}},
                                     {"isError", !r.ok}});
        return true;
    }

    if (isNotification) return false;
    *out = rpcErr(id, -32601, QStringLiteral("Method not found: %1").arg(method));
    return true;
}

}  // namespace

int McpServer::serve(bool allowWrites) {
    // SECURITY: an MCP client is a remote caller. Default to READ-ONLY so a
    // connected editor cannot run bash/write/rm on the user's machine; mutations
    // need an explicit opt-in. Non-interactive either way — a tool call cannot
    // block on an approval prompt nobody is watching.
    allowWrites = allowWrites || Config::boolean(QStringLiteral("mcp.allowWrites"), false);
    Permission::setMode(allowWrites ? PermMode::Auto : PermMode::ReadOnly);
    Permission::setInteractive(false);
    Tools::registerAll();

    StdoutJail jail;
    if (jail.channel() < 0) return 1;

    // Newline-delimited JSON-RPC on stdin (the MCP stdio transport). stdin is
    // untouched by the jail — only fd 1 moved.
    std::string line;
    while (std::getline(std::cin, line)) {
        const QByteArray raw = QByteArray::fromStdString(line).trimmed();
        if (raw.isEmpty()) continue;
        const QJsonObject msg = QJsonDocument::fromJson(raw).object();
        if (msg.isEmpty()) continue;

        QJsonObject resp;
        if (!handleRpc(msg, &resp)) continue;
        writeLine(jail.channel(), QJsonDocument(resp).toJson(QJsonDocument::Compact));
    }
    return 0;
}

}  // namespace odv
