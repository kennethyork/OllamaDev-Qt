#include "Agent.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtConcurrent/QtConcurrentMap>

#include "Config.h"
#include "Tools.h"
#include "Usage.h"

namespace odv {
namespace {

const char* kBasePrompt =
    "You are OllamaDev, a helpful AI coding assistant running locally via Ollama.\n\n"
    "Talk to the user naturally in plain language. Answer questions, explain things,\n"
    "write code, and hold a normal conversation. Only use a tool when the task\n"
    "genuinely requires it: reading or editing files, running a shell command, or\n"
    "searching the codebase.\n\n"
    "If the user is just chatting or asking something you can answer directly, do\n"
    "NOT call a tool - simply reply with your answer. After a tool runs, read its\n"
    "result and explain it to the user in plain language.";

// Native mode ONLY. This must never describe a text protocol: showing one lures
// capable models (mistral, qwen, …) into emitting a pseudo-call like
// write{"file_path":…} as prose, which bypasses the structured tool_calls channel
// entirely — so the edit silently never happens.
const char* kToolPrompt =
    "You have tools available for acting on the project: read/view files, write/edit "
    "files, list and search (ls, glob, grep), and run shell commands (bash). They are "
    "wired directly into this conversation — invoke them with your native "
    "function/tool-calling. Do NOT write tool calls as text, JSON, or code blocks; "
    "just call the tool.\n\n"
    "Use a tool ONLY when the request needs it. For greetings, questions, or "
    "explanations, just reply in plain text — do not call a tool. To create or change "
    "a file you MUST call write/edit with the file's contents as an argument — do not "
    "paste the code into your message, and do not wrap file contents in ``` fences.";

const char* kPlanPrompt =
    "PLAN MODE: Investigate with READ-ONLY tools only — do NOT create/edit files or run "
    "mutating commands yet. When you have a concrete plan, call exit_plan_mode(plan: \"…\") "
    "and wait for the user's approval before implementing.";

// Re-serialize parsed calls into the shape Ollama expects to see replayed on the
// next request. ChatMessage::toolCalls is "raw, as the backend emitted them", and
// this is that wire shape: a model that gets its own calls back in a different
// form loses the thread between a call and its tool result.
QJsonArray rawCalls(const QVector<ToolCall>& calls) {
    QJsonArray out;
    for (const ToolCall& c : calls) {
        QJsonObject fn{{"name", c.name}, {"arguments", c.args}};
        QJsonObject entry{{"type", "function"}, {"function", fn}};
        if (!c.id.isEmpty()) entry.insert(QStringLiteral("id"), c.id);
        out.append(entry);
    }
    return out;
}

QString readIfPresent(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    const QString s = QString::fromUtf8(f.read(64 * 1024));  // bound it: this is prompt budget
    f.close();
    return s;
}

}  // namespace

Agent::Agent(const QString& backendId, const QString& model)
    : backendId_(backendId), model_(model) {
    Tools::registerAll();
    if (model_.isEmpty()) {
        if (BackendPtr be = Backends::get(backendId_)) model_ = be->defaultModel();
    }
    systemPrompt_ = buildSystemPrompt(QDir::currentPath());
}

QString Agent::buildSystemPrompt(const QString& projectRoot) const {
    QString prompt = Config::str(QStringLiteral("agents.systemPrompt"), QString());
    if (prompt.isEmpty()) prompt = QString::fromUtf8(kBasePrompt);

    if (Permission::mode() == PermMode::Plan)
        prompt += QStringLiteral("\n\n") + QString::fromUtf8(kPlanPrompt);

    // Project context, mirroring the PHP lookup order. The first one found wins —
    // a repo with both files means one is a symlink or a leftover, not two prompts.
    const QStringList candidates{QStringLiteral("OLLAMADEV.md"), QStringLiteral(".ollamadev.md"),
                                 QStringLiteral(".ollamadev")};
    const QString root = projectRoot.isEmpty() ? QDir::currentPath() : projectRoot;
    for (const QString& name : candidates) {
        const QString path = root + QLatin1Char('/') + name;
        if (!QFileInfo(path).isFile()) continue;
        const QString body = readIfPresent(path);
        if (body.trimmed().isEmpty()) continue;
        prompt += QStringLiteral("\n\nPROJECT CONTEXT (from %1):\n%2").arg(name, body);
        break;
    }

    // The CLI backends run their own agent with their own tool prompt; ours would
    // just be noise (and would describe tools they cannot call).
    const BackendPtr be = Backends::get(backendId_);
    if (be && !be->supportsNativeTools()) return prompt;

    return prompt + QStringLiteral("\n\n") + QString::fromUtf8(kToolPrompt);
}

void Agent::ensureSystem(QVector<ChatMessage>& messages) const {
    if (systemPrompt_.isEmpty()) return;
    if (!messages.isEmpty() && messages.first().role == QLatin1String("system")) {
        messages.first().content = systemPrompt_;
        return;
    }
    ChatMessage sys;
    sys.role = QStringLiteral("system");
    sys.content = systemPrompt_;
    messages.prepend(sys);
}

ChatTurn Agent::turn(QVector<ChatMessage>& messages, const StreamSink& sink,
                     const CancelToken& cancel) {
    ChatTurn t;
    const BackendPtr be = Backends::get(backendId_);
    if (!be) {
        t.error = QStringLiteral("unknown backend: %1").arg(backendId_);
        return t;
    }
    if (cancel.cancelled()) {
        t.error = QStringLiteral("cancelled");
        return t;
    }

    ensureSystem(messages);
    const QJsonArray schemas = be->supportsNativeTools() ? Tools::schemas() : QJsonArray();
    t = be->chat(model_, messages, schemas, sink, cancel);

    // No text-parsing fallback by design: a model that cannot do native tool calls
    // is an error the user fixes by picking a tool-capable model, not something we
    // paper over by scraping its prose for pseudo-calls.
    if (t.toolsUnsupported) {
        t.ok = false;
        if (t.error.isEmpty())
            t.error = QStringLiteral(
                          "model '%1' does not support tool calling — switch to a tool-capable "
                          "model (check `ollama show %1 | grep capabilities`)")
                          .arg(model_);
        return t;
    }
    if (!t.ok) return t;

    // Meter real token usage from the /api/chat counts. Covers the REPL agent
    // path, one-shots, and crew coders — every route that runs a native turn.
    if (t.promptTokens > 0 || t.evalTokens > 0)
        Usage::record(model_, t.promptTokens, t.evalTokens);

    ChatMessage m;
    m.role = QStringLiteral("assistant");
    m.content = t.content;
    m.thinking = t.thinking;
    if (!t.calls.isEmpty()) m.toolCalls = rawCalls(t.calls);
    messages.append(m);
    return t;
}

namespace {
// The one argument that says what a call is actually DOING. A watcher wants
// "src/Parser.cpp", not the whole JSON blob.
QString describeCall(const ToolCall& c) {
    for (const char* k : {"file_path", "path", "command", "pattern", "expr", "question", "branch",
                          "src", "message", "bg_id", "name"}) {
        const QJsonValue v = c.args.value(QLatin1String(k));
        if (v.isString() && !v.toString().isEmpty()) {
            QString s = v.toString().simplified();
            if (s.size() > 60) s = s.left(57) + QStringLiteral("…");
            return s;
        }
    }
    return {};
}
}  // namespace

void Agent::executeCalls(const QVector<ToolCall>& calls, QVector<ChatMessage>& messages) {
    if (calls.isEmpty()) return;
    Tools::registerAll();

    // thread_local does NOT propagate into the pool threads below, and a pool
    // thread may still carry a root from some other crew coder's earlier task. So
    // every worker explicitly adopts THIS thread's root (or explicitly clears it),
    // which keeps a parallel crew's read-only tools resolving inside their own
    // sandbox instead of a sibling's.
    const bool rooted = Tools::hasThreadRoot();
    const QString root = rooted ? Tools::threadRoot() : QString();

    auto runOne = [rooted, root](const ToolCall& c) -> ToolResult {
        Tools::setThreadRoot(rooted ? root : QString());
        return Tools::run(c.name, c.args);
    };

    auto emitResult = [&messages](const ToolCall& c, const ToolResult& r) {
        ChatMessage m;
        m.role = QStringLiteral("tool");
        m.toolName = c.name;
        m.toolCallId = c.id;
        // A failure is content, not an exception: the model needs to READ why the
        // tool refused in order to correct itself on the next turn.
        m.content = r.ok ? r.output : (r.error.isEmpty() ? QStringLiteral("tool failed") : r.error);
        if (m.content.isEmpty()) m.content = QStringLiteral("(no output)");
        messages.append(m);
    };

    QVector<ToolCall> batch;  // pending consecutive read-only calls
    auto flush = [&]() {
        if (batch.isEmpty()) return;
        if (batch.size() == 1) {
            emitResult(batch.first(), runOne(batch.first()));
        } else {
            const QVector<ToolResult> results =
                QtConcurrent::blockingMapped<QVector<ToolResult>>(batch, runOne);
            for (int i = 0; i < batch.size(); ++i) emitResult(batch.at(i), results.at(i));
        }
        batch.clear();
    };

    for (const ToolCall& c : calls) {
        const ToolDef* def = Tools::find(c.name);
        // An unknown tool is treated as mutating (serial): we cannot prove it is
        // safe to reorder, and Tools::run will turn it into a "not a valid tool"
        // message anyway.
        const bool serial = !def || def->mutates;
        if (serial) {
            flush();  // everything the model asked for BEFORE this write must have run
            emitResult(c, Tools::run(c.name, c.args));
            continue;
        }
        batch.append(c);
    }
    flush();
}

QString Agent::loop(QVector<ChatMessage>& messages, int maxIterations, const StreamSink& sink,
                    const CancelToken& cancel, const Interject& interject) {
    Tools::registerAll();
    const BackendPtr be = Backends::get(backendId_);
    if (!be) return QStringLiteral("Error: unknown backend '%1'").arg(backendId_);

    ensureSystem(messages);

    // ---- Coding-CLI branch (claude, codex, gemini, …) ----------------------
    // These backends are not "a model that emits tool calls" — they are a whole
    // agent in a subprocess. They read files, edit them and run commands
    // THEMSELVES, then print a result. So we must not feed them tool schemas
    // (there is no tool_calls channel to receive them, and advertising tools we
    // would then execute a second time would double-apply every edit), and we must
    // not loop: by the time the process exits, the work is already done on disk.
    // One prompt in, one final answer out.
    if (!be->supportsNativeTools()) {
        const ChatTurn t = be->chat(model_, messages, QJsonArray(), sink, cancel);
        if (!t.ok)
            return QStringLiteral("Error: %1")
                .arg(t.error.isEmpty() ? QStringLiteral("backend '%1' failed").arg(backendId_)
                                       : t.error);
        ChatMessage m;
        m.role = QStringLiteral("assistant");
        m.content = t.content;
        m.thinking = t.thinking;
        messages.append(m);
        return t.content;
    }

    // ---- Native tool-calling loop ------------------------------------------
    const int cap = qMax(1, maxIterations);
    QString last;
    for (int i = 0; i < cap; ++i) {
        // Cancellation is checked BETWEEN iterations only — never mid-tool — so a
        // cancelled agent never leaves a half-written file behind.
        if (cancel.cancelled())
            return last.isEmpty() ? QStringLiteral("(cancelled)")
                                  : last + QStringLiteral("\n\n[cancelled]");

        // A human's word gets in here, before the model is asked anything, so it
        // lands on the NEXT decision rather than being ignored until the end.
        if (interject) {
            const QString word = interject();
            if (!word.trimmed().isEmpty()) {
                ChatMessage m;
                m.role = QStringLiteral("user");
                m.content = word;
                messages.append(m);
            }
        }

        const ChatTurn t = turn(messages, sink, cancel);
        if (!t.ok) {
            const QString err = QStringLiteral("Error: %1").arg(
                t.error.isEmpty() ? QStringLiteral("the model turn failed") : t.error);
            return last.isEmpty() ? err : last + QStringLiteral("\n\n") + err;
        }
        if (!t.content.trimmed().isEmpty()) last = t.content;

        if (t.calls.isEmpty()) return t.content;  // no tools requested => the model is done

        // Report the ACTION before it happens, so a watcher sees "editing Parser.cpp"
        // while it is being edited rather than after the fact.
        if (sink.onTool)
            for (const ToolCall& c : t.calls) sink.onTool(c.name, describeCall(c));

        executeCalls(t.calls, messages);
    }

    return last + QStringLiteral("\n\n[stopped after %1 iterations — the model was still calling "
                                 "tools; re-run to continue]")
                      .arg(cap);
}

}  // namespace odv
