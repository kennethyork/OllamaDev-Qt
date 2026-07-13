#include "SubAgent.h"

#include <QJsonArray>
#include <algorithm>

#include "Agent.h"
#include "AgentDefs.h"
#include "Backend.h"
#include "Config.h"

namespace odv {
namespace {

// Current nesting level for THIS thread. A subagent runs synchronously inside a
// (serial, mutating) `task` tool call on the calling thread, and any `task` it
// issues recurses on the same thread — so a thread_local counter tracks depth
// correctly without a shared global that concurrent crew coders would race on.
thread_local int g_depth = 0;

QString argStr(const QJsonObject& a, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        const QJsonValue v = a.value(QLatin1String(k));
        if (v.isString()) return v.toString();
    }
    return {};
}

}  // namespace

QString SubAgent::run(const QString& task, int) {
    return run(QJsonObject{{QStringLiteral("prompt"), task}});
}

QString SubAgent::run(const QJsonObject& p) {
    const QString prompt = argStr(p, {"prompt", "task"}).trimmed();
    if (prompt.isEmpty())
        return QStringLiteral("missing prompt (need a 'prompt' or 'task' parameter)");

    if (g_depth >= kMaxDepth)
        return QStringLiteral("Sub-agent depth limit reached (max %1); refusing to nest further. "
                              "Complete this task directly.")
            .arg(kMaxDepth);

    const QString context = argStr(p, {"context"}).trimmed();
    int maxIter = p.value(QStringLiteral("max_iterations")).toInt(
        p.value(QStringLiteral("iterations")).toInt(6));
    maxIter = std::clamp(maxIter, 1, 12);

    // A custom persona (.ollamadev/agents/<name>.md) can supply a prompt, model,
    // permission, and tool list. An unknown name is a hard error rather than a
    // silent fall-through, so a typo does not quietly run the default agent.
    AgentDef def;
    const QString at = argStr(p, {"agent_type", "agent", "subagent_type"}).trimmed();
    if (!at.isEmpty()) {
        def = AgentDefs::get(at);
        if (def.isNull())
            return QStringLiteral("No such agent type: %1. List them with `ollamadev agents`, or "
                                  "omit agent_type.")
                .arg(at);
    }

    // Model: the persona's pick, else the configured/backend default. There is no
    // process-wide "current session model" in this port, so config is the source.
    const QString backendId = Config::str(QStringLiteral("model.backend"), QStringLiteral("ollama"));
    QString model = def.model.isEmpty() ? Config::str(QStringLiteral("ollama.defaultModel"))
                                        : def.model;
    if (model.isEmpty()) {
        if (const BackendPtr be = Backends::get(backendId)) model = be->defaultModel();
    }

    // Permission policy. A sub-agent is READ-ONLY by default and is NEVER more
    // permissive than its parent: a plan/read-only parent forces read-only, and an
    // ask parent caps an "auto" request back down to ask (nobody can field the
    // approval prompt in a non-interactive sub-run anyway).
    const PermMode parentMode = Permission::mode();
    const bool parentInteractive = Permission::interactive();

    QString subName = Config::str(QStringLiteral("agents.subagentPermission"),
                                  QStringLiteral("readonly")).toLower();
    if (!def.permission.isEmpty()) subName = def.permission.toLower();
    const QString req = argStr(p, {"permission"}).trimmed().toLower();
    if (p.value(QStringLiteral("allow_writes")).toBool() || req == QLatin1String("auto"))
        subName = QStringLiteral("auto");
    else if (req == QLatin1String("ask") || req == QLatin1String("readonly") ||
             req == QLatin1String("auto"))
        subName = req;
    if (parentMode == PermMode::ReadOnly || parentMode == PermMode::Plan)
        subName = QStringLiteral("readonly");
    if (parentMode == PermMode::Ask && subName == QLatin1String("auto"))
        subName = QStringLiteral("ask");
    const PermMode subMode = Permission::modeFromName(subName);

    Agent sub(backendId, model);
    // The persona body becomes the system prompt; an unset persona keeps the
    // agent's own default. A tools allowlist is advertised to the model as a soft
    // constraint (this port has no per-agent hard tool gate).
    if (!def.prompt.isEmpty()) {
        QString persona = def.prompt;
        if (!def.tools.isEmpty())
            persona += QStringLiteral("\n\nYou may use ONLY these tools: %1.")
                           .arg(def.tools.join(QStringLiteral(", ")));
        sub.setSystemPrompt(persona);
    }

    QString userContent = QStringLiteral("Task: %1").arg(prompt);
    if (!context.isEmpty()) userContent += QStringLiteral("\n\nContext:\n%1").arg(context);
    userContent += QStringLiteral("\n\nWork through this using tools as needed, then give a concise "
                                  "final answer in plain text.");

    QVector<ChatMessage> messages;
    ChatMessage um;
    um.role = QStringLiteral("user");
    um.content = userContent;
    messages.append(um);

    // The sub-agent inherits the caller thread's Tools::threadRoot() (a crew
    // coder's sandbox, or the CLI's project root) — its file tools resolve inside
    // the same boundary. We only narrow the permission MODE, non-interactively,
    // then restore the parent's policy afterwards.
    ++g_depth;
    Permission::setMode(subMode);
    Permission::setInteractive(false);

    StreamSink silent;  // a delegated run streams nothing to the parent's terminal
    CancelToken cancel;
    QString result = sub.loop(messages, maxIter, silent, cancel);

    --g_depth;
    Permission::setMode(parentMode);
    Permission::setInteractive(parentInteractive);

    result = result.trimmed();
    if (result.isEmpty()) return QStringLiteral("Sub-agent produced no output.");
    return result;
}

QVector<ToolDef> SubAgent::tools() {
    ToolDef task;
    task.name = QStringLiteral("task");
    task.description =
        QStringLiteral("Delegate a focused sub-task to a nested agent that works through it with "
                       "its own tools and returns a concise result. Read-only by default. Optional "
                       "agent_type selects a persona from .ollamadev/agents/<name>.md.");
    task.parameters = QJsonObject{
        {"type", "object"},
        {"properties",
         QJsonObject{
             {"prompt", QJsonObject{{"type", "string"},
                                    {"description", "The sub-task to carry out"}}},
             {"context", QJsonObject{{"type", "string"},
                                     {"description", "Optional background the sub-agent should know"}}},
             {"agent_type", QJsonObject{{"type", "string"},
                                        {"description", "Optional persona name (see `ollamadev agents`)"}}},
             {"permission", QJsonObject{{"type", "string"},
                                        {"description", "readonly | ask | auto (never exceeds the "
                                                        "parent's permission)"}}}}},
        {"required", QJsonArray{QStringLiteral("prompt")}}};
    // mutates=true so it runs SERIALLY, not fanned out with read-only siblings:
    // the sub-run mutates process-global Permission mode for its duration, which is
    // only safe when nothing else runs concurrently.
    task.mutates = true;
    task.fn = [](const QJsonObject& a) -> ToolResult {
        return ToolResult{true, SubAgent::run(a), QString()};
    };

    ToolDef subagent = task;
    subagent.name = QStringLiteral("subagent");
    ToolDef delegate = task;
    delegate.name = QStringLiteral("delegate");

    return {task, subagent, delegate};
}

}  // namespace odv
