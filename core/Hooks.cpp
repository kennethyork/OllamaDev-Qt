#include "Hooks.h"

#include "Plugins.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>

#include "Config.h"
#include "Json.h"
#include "Tools.h"

namespace odv {
namespace {

// Exit codes we synthesise when the hook never got to report one of its own.
// Both are non-zero on purpose: PreToolUse fails CLOSED, so "could not run it"
// must land on the blocking side of the branch.
constexpr int kCouldNotSpawn = 127;  // shell convention: command not found
constexpr int kTimedOut = 124;       // shell convention: timeout(1)

constexpr int kDefaultTimeoutMs = 30000;
constexpr int kMaxResultBytes = 4000;  // how much tool output a PostToolUse hook sees

QString homePath() {
    const QString h = QDir::homePath();
    return h.isEmpty() ? QStringLiteral("/tmp") : h;
}

// The ONLY files a hook may be declared in. A repo-local ./.ollamadev.json is
// absent by design — see the class comment in Hooks.h.
QStringList trustedFiles() {
    return {homePath() + QStringLiteral("/.ollamadev/config.json"),
            homePath() + QStringLiteral("/.config/ollamadev/config.json"),
            homePath() + QStringLiteral("/.ollamadev/ade-prefs.json")};
}

QJsonObject readJsonObject(const QString& path) {
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return {};
    const QByteArray raw = f.readAll();
    f.close();
    return json::objectFrom(QString::fromUtf8(raw));
}

struct TrustedCache {
    QMutex mutex;
    QJsonObject cfg;
    QList<QDateTime> stamps;  // parallel to trustedFiles()
    bool loaded = false;
};

TrustedCache& cache() {
    static TrustedCache c;
    return c;
}

// The merged HOME config, reloaded whenever one of the trusted files changes on
// disk. Tools::run() consults this on every single call (including read-only
// tools fanned out across crew worker threads), so it cannot re-parse three JSON
// files each time — but it also cannot cache forever, or a hook added by another
// process would never take effect in a long-lived REPL. Three stats is the
// compromise.
QJsonObject trustedConfig() {
    const QStringList files = trustedFiles();
    QList<QDateTime> now;
    now.reserve(files.size());
    for (const QString& p : files) now.append(QFileInfo(p).lastModified());

    TrustedCache& c = cache();
    QMutexLocker guard(&c.mutex);
    if (c.loaded && c.stamps == now) return c.cfg;

    QJsonObject merged;
    for (int i = 0; i < files.size(); ++i) {
        QJsonObject layer = readJsonObject(files.at(i));
        // ade-prefs.json holds FLAT dotted keys ("hooks.PreToolUse": [...]).
        if (files.at(i).endsWith(QLatin1String("ade-prefs.json"))) layer = json::expandDotted(layer);
        merged = json::mergeDeep(merged, layer);
    }
    c.cfg = merged;
    c.stamps = now;
    c.loaded = true;
    return merged;
}

QString trimmedString(const QJsonValue& v) {
    return v.isString() ? v.toString().trimmed() : QString();
}

// Does `matcher` (a regex) select `subject`? An empty matcher, or an empty
// subject, means "always".
bool matches(const QString& matcher, const QString& subject) {
    if (matcher.isEmpty() || subject.isEmpty()) return true;
    const QRegularExpression rx(matcher, QRegularExpression::CaseInsensitiveOption);
    // An INVALID pattern fails CLOSED (the hook applies). Silently dropping a
    // filter the user believed was guarding them is the worse failure; the broken
    // pattern is theirs to fix, and they will see it fire on everything.
    if (!rx.isValid()) return true;
    return rx.match(subject).hasMatch();
}

// Hooks from ENABLED plugins, for the EXECUTION path.
//
// Kept as its own function and appended by EVERY return path of commandsFor()
// below, because the obvious way to write this — one loop at the bottom — is
// wrong: commandsFor() returns early when the user has no config hooks for the
// event, so a plugin's hook would then fire ONLY for users who happened to
// already have a config hook of their own. Which is exactly the bug that was
// here, and exactly the kind a happy-path test never finds.
QStringList pluginCommandsFor(const QString& event, const QString& subject) {
    QStringList out;
    for (const PluginHook& h : Plugins::hooksFor(event)) {
        const QString cmd = h.command.trimmed();
        if (cmd.isEmpty() || !matches(h.matcher.trimmed(), subject)) continue;
        out << cmd;
    }
    return out;
}

// Commands configured for an event, after matcher filtering. Config hooks first,
// then the enabled plugins' — they rank with the HOME config, not the project
// file, because the user installed the plugin and then explicitly said yes to it
// having been shown the exact commands. A repo you merely cloned never gets that.
QStringList commandsFor(const QString& event, const QString& subject) {
    const QJsonValue cfg = json::at(trustedConfig(), QStringLiteral("hooks.") + event);
    QStringList out;

    // A bare string is the one-hook shorthand.
    if (cfg.isString()) {
        const QString s = cfg.toString().trimmed();
        if (!s.isEmpty()) out << s;
        return out + pluginCommandsFor(event, subject);
    }
    if (!cfg.isArray()) return out + pluginCommandsFor(event, subject);

    for (const QJsonValue& h : cfg.toArray()) {
        if (h.isString()) {
            const QString s = h.toString().trimmed();
            if (!s.isEmpty()) out << s;
            continue;
        }
        if (!h.isObject()) continue;
        const QJsonObject o = h.toObject();
        QString cmd = trimmedString(o.value(QStringLiteral("command")));
        if (cmd.isEmpty()) cmd = trimmedString(o.value(QStringLiteral("cmd")));
        if (cmd.isEmpty()) continue;
        QString matcher = trimmedString(o.value(QStringLiteral("matcher")));
        if (matcher.isEmpty()) matcher = trimmedString(o.value(QStringLiteral("match")));
        if (!matches(matcher, subject)) continue;
        out << cmd;
    }
    return out + pluginCommandsFor(event, subject);
}

int timeoutMs() {
    const QJsonValue v = json::at(trustedConfig(), QStringLiteral("hooks.timeoutMs"));
    const int ms = v.isDouble() ? v.toInt() : (v.isString() ? v.toString().toInt() : 0);
    return ms > 0 ? ms : kDefaultTimeoutMs;
}

struct HookRun {
    QString output;
    int code = 0;
};

// Run one hook command through the shell, with `stdinText` on stdin and `extra`
// added to the environment.
HookRun exec(const QString& cmd, const QByteArray& stdinText, const QStringList& extra) {
    // Reentrancy guard: a hook that itself runs `ollamadev` would fire the same
    // hooks again, without bound. One level deep they are suppressed — and this
    // returns code 0 (ALLOW) deliberately: the outer invocation already passed the
    // gate, so re-gating its own child would deadlock the user out of their tools.
    const int depth = qEnvironmentVariableIntValue("OLLAMADEV_HOOK_DEPTH");
    if (depth >= 1) return {};

    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("OLLAMADEV_HOOK_DEPTH"), QString::number(depth + 1));
    for (const QString& e : extra) {
        const int eq = e.indexOf(QLatin1Char('='));
        if (eq > 0) env.insert(e.left(eq), e.mid(eq + 1));
    }
    p.setProcessEnvironment(env);

    // A hook is a subprocess that may EDIT FILES. In a crew coder it must land in
    // that coder's sandbox, not in the user's real project — threadRoot() is the
    // per-thread root and falls back to the process cwd when none was set.
    p.setWorkingDirectory(Tools::threadRoot());

    p.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), cmd});
    if (!p.waitForStarted(5000)) return {QString(), kCouldNotSpawn};

    if (!stdinText.isEmpty()) p.write(stdinText);
    p.closeWriteChannel();

    if (!p.waitForFinished(timeoutMs())) {
        // Our own child, killed by handle. Never a pkill by name: two hooks with
        // the same command line may be running in parallel crew coders.
        p.kill();
        p.waitForFinished(2000);
        return {QStringLiteral("hook timed out"), kTimedOut};
    }

    QString out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    const QString err = QString::fromUtf8(p.readAllStandardError()).trimmed();
    if (!err.isEmpty()) out += (out.isEmpty() ? QString() : QStringLiteral("\n")) + err;

    if (p.exitStatus() != QProcess::NormalExit) return {out, kTimedOut};
    return {out, p.exitCode()};
}

QString shellQuote(const QString& s) {
    QString q = s;
    q.replace(QLatin1String("'"), QLatin1String("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

QByteArray encodeJson(const QJsonObject& o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

}  // namespace

// ---------------------------------------------------------------------- Hooks

QStringList Hooks::knownEvents() {
    return {QStringLiteral("PreToolUse"),   QStringLiteral("PostToolUse"),
            QStringLiteral("UserPromptSubmit"), QStringLiteral("SessionStart"),
            QStringLiteral("Stop"),         QStringLiteral("PreCompact"),
            QStringLiteral("SubagentStop"), QStringLiteral("Notification"),
            QStringLiteral("beforePrompt"), QStringLiteral("afterEdit")};
}

QString Hooks::normalizeEvent(const QString& e) {
    for (const QString& k : knownEvents())
        if (k.compare(e, Qt::CaseInsensitive) == 0) return k;
    return {};
}

// The hooks DECLARED IN CONFIG. Deliberately does NOT include plugin hooks.
//
// This is the EDITING view: `hooks add` and `hooks remove` read it, modify it, and
// write the result back to config. Mixing plugin-contributed hooks in here means
// the next `hooks add` reads a plugin's hook as if it were the user's own and
// bakes it permanently into their config file — where it then survives the plugin
// being disabled or removed, because it is no longer the plugin's hook at all.
// That is not a hypothetical: it happened, and it is why this comment exists.
//
// Plugin hooks are live in commandsFor() (the execution path) and shown as a
// separate, read-only section by renderConfigured().
QVector<Hooks::Hook> Hooks::listFor(const QString& event) {
    const QString ev = normalizeEvent(event);
    QVector<Hook> out;
    if (ev.isEmpty()) return out;

    const QJsonValue cfg = json::at(trustedConfig(), QStringLiteral("hooks.") + ev);
    if (cfg.isString()) {
        const QString s = cfg.toString().trimmed();
        if (!s.isEmpty()) out.append({s, QString()});
        return out;
    }
    if (!cfg.isArray()) return out;

    for (const QJsonValue& h : cfg.toArray()) {
        if (h.isString()) {
            const QString s = h.toString().trimmed();
            if (!s.isEmpty()) out.append({s, QString()});
            continue;
        }
        if (!h.isObject()) continue;
        const QJsonObject o = h.toObject();
        QString cmd = trimmedString(o.value(QStringLiteral("command")));
        if (cmd.isEmpty()) cmd = trimmedString(o.value(QStringLiteral("cmd")));
        if (cmd.isEmpty()) continue;
        QString matcher = trimmedString(o.value(QStringLiteral("matcher")));
        if (matcher.isEmpty()) matcher = trimmedString(o.value(QStringLiteral("match")));
        out.append({cmd, matcher});
    }
    return out;
}

bool Hooks::add(const QString& event, const QString& command, const QString& matcher) {
    const QString ev = normalizeEvent(event);
    if (ev.isEmpty() || command.trimmed().isEmpty()) return false;

    // The persisted list is the MERGED view, so a hook the user hand-wrote into
    // config.json survives an `add` (ade-prefs overlays config.json, and an array
    // overlay replaces rather than merges — writing only the new entry would drop
    // the old ones).
    QJsonArray list;
    for (const Hook& h : listFor(ev)) {
        QJsonObject e{{QStringLiteral("command"), h.command}};
        if (!h.matcher.isEmpty()) e.insert(QStringLiteral("matcher"), h.matcher);
        list.append(e);
    }
    QJsonObject e{{QStringLiteral("command"), command.trimmed()}};
    if (!matcher.trimmed().isEmpty()) e.insert(QStringLiteral("matcher"), matcher.trimmed());
    list.append(e);

    // setPref writes ~/.ollamadev/ade-prefs.json — a HOME file, so what we write
    // stays inside the trusted set we read from.
    Config::setPref(QStringLiteral("hooks.") + ev, list);
    return true;
}

bool Hooks::removeAt(const QString& event, int index) {
    const QString ev = normalizeEvent(event);
    if (ev.isEmpty()) return false;
    const QVector<Hook> cur = listFor(ev);
    if (index < 0 || index >= cur.size()) return false;

    QJsonArray list;
    for (int i = 0; i < cur.size(); ++i) {
        if (i == index) continue;
        QJsonObject e{{QStringLiteral("command"), cur.at(i).command}};
        if (!cur.at(i).matcher.isEmpty())
            e.insert(QStringLiteral("matcher"), cur.at(i).matcher);
        list.append(e);
    }
    Config::setPref(QStringLiteral("hooks.") + ev, list);
    return true;
}

bool Hooks::preToolUse(const QString& tool, const QJsonObject& params, QString* reason) {
    const QStringList cmds = commandsFor(QStringLiteral("PreToolUse"), tool);
    if (cmds.isEmpty()) return false;

    const QByteArray payload =
        encodeJson({{QStringLiteral("tool"), tool}, {QStringLiteral("input"), params}});
    const QStringList env{QStringLiteral("OLLAMADEV_TOOL_NAME=") + tool,
                          QStringLiteral("OLLAMADEV_TOOL_INPUT=") + QString::fromUtf8(payload),
                          QStringLiteral("OLLAMADEV_EVENT=PreToolUse")};

    for (const QString& cmd : cmds) {
        const HookRun r = exec(cmd, payload, env);
        if (r.code == 0) continue;
        if (reason) {
            *reason = r.output.isEmpty()
                          ? QStringLiteral("PreToolUse hook blocked '%1' (exit %2)")
                                .arg(tool)
                                .arg(r.code)
                          : r.output;
        }
        return true;  // blocked
    }
    return false;
}

void Hooks::postToolUse(const QString& tool, const QJsonObject& params, const QString& result) {
    const QStringList cmds = commandsFor(QStringLiteral("PostToolUse"), tool);
    if (cmds.isEmpty()) return;

    const QByteArray payload = encodeJson({{QStringLiteral("tool"), tool},
                                           {QStringLiteral("input"), params},
                                           {QStringLiteral("result"), result.left(kMaxResultBytes)}});
    const QStringList env{
        QStringLiteral("OLLAMADEV_TOOL_NAME=") + tool,
        QStringLiteral("OLLAMADEV_TOOL_INPUT=") + QString::fromUtf8(encodeJson(params)),
        QStringLiteral("OLLAMADEV_EVENT=PostToolUse")};

    for (const QString& cmd : cmds) exec(cmd, payload, env);  // informational: exit code ignored
}

void Hooks::event(const QString& name, const QJsonObject& payload) {
    const QString ev = normalizeEvent(name);
    if (ev.isEmpty()) return;
    const QString subject = payload.value(QStringLiteral("_subject")).toString();
    const QStringList cmds = commandsFor(ev, subject);
    if (cmds.isEmpty()) return;

    const QStringList env{QStringLiteral("OLLAMADEV_EVENT=") + ev};
    const QByteArray body = encodeJson(payload);
    for (const QString& cmd : cmds) exec(cmd, body, env);
}

void Hooks::run(const QString& event, const QStringList& args) {
    const QString ev = normalizeEvent(event);
    if (ev.isEmpty()) return;
    const QStringList cmds = commandsFor(ev, QString());
    if (cmds.isEmpty()) return;

    for (const QString& cmd : cmds) {
        QString full = cmd.trimmed();
        for (const QString& a : args) full += QLatin1Char(' ') + shellQuote(a);
        QStringList env{QStringLiteral("OLLAMADEV_EVENT=") + ev};
        if (ev == QLatin1String("afterEdit") && !args.isEmpty())
            env << QStringLiteral("OLLAMADEV_EDITED_FILES=") + args.join(QLatin1Char(' '));
        exec(full, {}, env);
    }
}

QString Hooks::renderConfigured() {
    QString out = QStringLiteral("\n  Hooks:\n");
    bool any = false;
    for (const QString& ev : knownEvents()) {
        const QVector<Hook> list = listFor(ev);
        if (list.isEmpty()) continue;
        any = true;
        out += QStringLiteral("  ") + ev + QLatin1Char('\n');
        for (int i = 0; i < list.size(); ++i) {
            out += QStringLiteral("    %1: %2").arg(i).arg(list.at(i).command);
            if (!list.at(i).matcher.isEmpty())
                out += QStringLiteral(" [match: %1]").arg(list.at(i).matcher);
            out += QLatin1Char('\n');
        }
    }
    if (!any) out += QStringLiteral("  (none configured)\n");

    // Plugin hooks are LIVE but are not yours to edit here — they belong to the
    // plugin, and they go away when you disable it. Shown separately and without
    // an index, so nobody tries to `hooks remove` one and finds it still firing.
    QString fromPlugins;
    for (const QString& ev : knownEvents()) {
        for (const PluginHook& h : Plugins::hooksFor(ev)) {
            fromPlugins += QStringLiteral("    %1: %2").arg(ev, h.command);
            if (!h.matcher.isEmpty()) fromPlugins += QStringLiteral(" [match: %1]").arg(h.matcher);
            fromPlugins += QLatin1Char('\n');
        }
    }
    if (!fromPlugins.isEmpty())
        out += QStringLiteral("\n  From enabled plugins (disable the plugin to stop these):\n") +
               fromPlugins;

    out += QStringLiteral(
        "  add: hooks add <event> <command> [--match <regex>]  ·  remove: hooks remove <event> "
        "<index>\n");
    out += QStringLiteral("  events: ") + knownEvents().join(QStringLiteral(", ")) +
           QLatin1Char('\n');
    out += QStringLiteral(
        "  PreToolUse blocks the tool when the hook exits non-zero (its output is the reason).\n"
        "  Hooks come from your HOME config only — a cloned repo cannot plant one.\n");
    return out;
}

QString Hooks::editorCommand(const QStringList& words) {
    const QString sub = words.value(0).toLower();
    if (sub.isEmpty() || sub == QLatin1String("list")) return renderConfigured();

    if (sub == QLatin1String("add")) {
        const QString ev = normalizeEvent(words.value(1));
        if (ev.isEmpty())
            return QStringLiteral("Unknown event. Known: ") +
                   knownEvents().join(QStringLiteral(", ")) + QLatin1Char('\n');

        QStringList rest = words.mid(2);
        QString matcher;
        const int m = rest.indexOf(QStringLiteral("--match"));
        if (m >= 0) {
            matcher = rest.value(m + 1);
            rest.remove(m, m + 1 < rest.size() ? 2 : 1);
        }
        const QString cmd = rest.join(QLatin1Char(' ')).trimmed();
        if (cmd.isEmpty())
            return QStringLiteral("Usage: hooks add <event> <command> [--match <regex>]\n");
        return add(ev, cmd, matcher) ? QStringLiteral("✓ added %1 hook\n").arg(ev) + renderConfigured()
                                     : QStringLiteral("could not add hook\n");
    }

    if (sub == QLatin1String("remove") || sub == QLatin1String("rm") ||
        sub == QLatin1String("delete")) {
        const QString ev = normalizeEvent(words.value(1));
        if (ev.isEmpty()) return QStringLiteral("Usage: hooks remove <event> <index>\n");
        bool ok = false;
        const int idx = words.value(2).toInt(&ok);
        if (!ok) return QStringLiteral("Usage: hooks remove <event> <index>\n");
        return removeAt(ev, idx)
                   ? QStringLiteral("✓ removed %1 hook #%2\n").arg(ev).arg(idx) + renderConfigured()
                   : QStringLiteral("no hook at that index (see: hooks list)\n");
    }

    return QStringLiteral(
        "Usage: hooks [list | add <event> <command> [--match <regex>] | remove <event> <index>]\n");
}

// ------------------------------------------------------------------- UserCmds

namespace {

// Project first, so a repo can override a personal command of the same name.
QStringList cmdDirs() {
    return {QDir::current().filePath(QStringLiteral(".ollamadev/commands")),
            homePath() + QStringLiteral("/.ollamadev/commands")};
}

const QStringList& cmdExts() {
    static const QStringList e{QStringLiteral("md"), QStringLiteral("txt"),
                               QStringLiteral("prompt")};
    return e;
}

QString findCommandFile(const QString& name) {
    // The name indexes straight into a path, so anything that could climb out of
    // the commands directory ("../../.bashrc") is rejected outright.
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_.-]+$"));
    if (name.isEmpty() || !safe.match(name).hasMatch() || name.contains(QLatin1String("..")))
        return {};

    for (const QString& dir : cmdDirs()) {
        for (const QString& ext : cmdExts()) {
            const QString path = dir + QLatin1Char('/') + name + QLatin1Char('.') + ext;
            if (QFileInfo(path).isFile()) return path;
        }
    }
    return {};
}

}  // namespace

bool UserCmds::exists(const QString& name) {
    return !findCommandFile(name).isEmpty();
}

QString UserCmds::expand(const QString& name, const QString& args) {
    const QString path = findCommandFile(name);
    if (path.isEmpty()) return {};

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QString tpl = QString::fromUtf8(f.readAll());
    f.close();

    const QString trimmed = args.trimmed();
    const QStringList words = trimmed.split(QRegularExpression(QStringLiteral("\\s+")),
                                            Qt::SkipEmptyParts);

    // $ARGS first: it is the longest token, and a naive numbered pass would eat
    // nothing of it, but doing it after would let a substituted argument that
    // itself contains "$1" get re-expanded.
    tpl.replace(QStringLiteral("$ARGS"), trimmed);

    static const QRegularExpression positional(QStringLiteral("\\$(\\d+)"));
    QString out;
    int last = 0;
    auto it = positional.globalMatch(tpl);
    while (it.hasNext()) {
        const auto m = it.next();
        out += tpl.mid(last, m.capturedStart() - last);
        const int i = m.captured(1).toInt();
        if (i >= 1 && i <= words.size()) out += words.at(i - 1);
        last = m.capturedEnd();
    }
    out += tpl.mid(last);

    while (out.endsWith(QLatin1Char('\n'))) out.chop(1);
    return out;
}

QStringList UserCmds::listAll() {
    QStringList names;
    for (const QString& dir : cmdDirs()) {
        QDir d(dir);
        if (!d.exists()) continue;
        for (const QFileInfo& fi : d.entryInfoList(QDir::Files)) {
            if (!cmdExts().contains(fi.suffix().toLower())) continue;
            const QString base = fi.completeBaseName();
            if (!base.isEmpty() && !names.contains(base)) names << base;
        }
    }
    names.sort();
    return names;
}

QString UserCmds::render() {
    const QStringList names = listAll();
    if (names.isEmpty()) {
        return QStringLiteral(
            "\n  No custom commands found.\n"
            "  Create one at ~/.ollamadev/commands/NAME.md (a prompt template; use $ARGS\n"
            "  or $1 $2 for arguments), then run /NAME.\n");
    }
    QString out = QStringLiteral("\n  Custom commands (%1):\n").arg(names.size());
    for (const QString& n : names) out += QStringLiteral("  /") + n + QLatin1Char('\n');
    out += QStringLiteral("  Edit them under ~/.ollamadev/commands or ./.ollamadev/commands\n");
    return out;
}

}  // namespace odv
