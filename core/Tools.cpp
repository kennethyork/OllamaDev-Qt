// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Tools.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QThread>
#include <QVector>
#include <atomic>
#include <cmath>

#ifndef Q_OS_WIN
#include <csignal>
#endif

#include "CodeIndex.h"
#include "Config.h"
#include "GitFlow.h"
#include "Hooks.h"
#include "Mcp.h"
#include "Memory.h"
#include "Skills.h"
#include "SubAgent.h"
#include "WebSearch.h"

namespace odv {
namespace {

// ---------------------------------------------------------------- arg helpers

QString argStr(const QJsonObject& a, std::initializer_list<const char*> keys,
               const QString& def = QString()) {
    for (const char* k : keys) {
        const QJsonValue v = a.value(QLatin1String(k));
        if (v.isString()) return v.toString();
        if (v.isDouble()) return QString::number(v.toDouble());
    }
    return def;
}

bool argBool(const QJsonObject& a, std::initializer_list<const char*> keys, bool def = false) {
    for (const char* k : keys) {
        const QJsonValue v = a.value(QLatin1String(k));
        if (v.isBool()) return v.toBool();
        if (v.isDouble()) return v.toDouble() != 0.0;
        // Models routinely send booleans as the strings "true"/"1".
        if (v.isString()) {
            const QString s = v.toString().trimmed().toLower();
            if (s == "true" || s == "1" || s == "yes") return true;
            if (s == "false" || s == "0" || s == "no") return false;
        }
    }
    return def;
}

int argInt(const QJsonObject& a, std::initializer_list<const char*> keys, int def) {
    for (const char* k : keys) {
        const QJsonValue v = a.value(QLatin1String(k));
        if (v.isDouble()) return static_cast<int>(v.toDouble());
        if (v.isString()) {
            bool ok = false;
            const int n = v.toString().trimmed().toInt(&ok);
            if (ok) return n;
        }
    }
    return def;
}

ToolResult fail(const QString& msg) { return ToolResult{false, QString(), msg}; }
ToolResult okay(const QString& out) { return ToolResult{true, out, QString()}; }

// -------------------------------------------------------------- text helpers

// Local models habitually wrap a whole file's content in a markdown fence
// (```php … ```). Written verbatim the file starts with ``` and won't run. Strip
// a SINGLE fully-enclosing fence only, so a legit multi-block markdown file
// (which has more than one fence) is left untouched.
QString unfence(const QString& s) {
    const QString t = s.trimmed();
    if (t.count(QLatin1String("```")) != 2) return s;
    static const QRegularExpression rx(QStringLiteral("^```[^\\n]*\\n(.*)\\n```\\s*$"),
                                       QRegularExpression::DotMatchesEverythingOption);
    const auto m = rx.match(t);
    if (!m.hasMatch()) return s;
    return m.captured(1) + QLatin1Char('\n');
}

// Non-overlapping occurrence count (PHP substr_count semantics). QString::count()
// counts OVERLAPPING matches, which would misreport "how many places is this?".
int countOccurrences(const QString& hay, const QString& needle) {
    if (needle.isEmpty()) return 0;
    int n = 0;
    for (int i = hay.indexOf(needle); i >= 0; i = hay.indexOf(needle, i + needle.size())) ++n;
    return n;
}

// When an exact old_string match fails, weak models usually have the indentation
// or inter-token whitespace slightly wrong — the single most common edit-tool
// failure. Retry with a whitespace-flexible match: same non-space tokens in
// order, any run of whitespace between them. Applied ONLY when it matches
// EXACTLY ONCE; we never guess between candidate sites.
bool fuzzyFind(const QString& content, const QString& needle, int* pos, int* len) {
    const QString n = needle.trimmed();
    if (n.isEmpty()) return false;
    const QStringList parts = n.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return false;
    QStringList quoted;
    quoted.reserve(parts.size());
    for (const QString& p : parts) quoted << QRegularExpression::escape(p);
    const QRegularExpression rx(quoted.join(QStringLiteral("\\s+")));
    if (!rx.isValid()) return false;

    auto it = rx.globalMatch(content);
    if (!it.hasNext()) return false;
    const auto first = it.next();
    if (it.hasNext()) return false;  // ambiguous — refuse
    *pos = first.capturedStart();
    *len = first.capturedLength();
    return true;
}

// Directories we never walk for grep/glob: they are enormous, machine-generated,
// and never what the model is looking for.
const QStringList& skipDirs() {
    static const QStringList d{QStringLiteral(".git"),       QStringLiteral("node_modules"),
                               QStringLiteral("vendor"),     QStringLiteral(".ollamadev"),
                               QStringLiteral("build"),      QStringLiteral("dist"),
                               QStringLiteral(".venv"),      QStringLiteral("venv"),
                               QStringLiteral("__pycache__"), QStringLiteral(".cache")};
    return d;
}

bool underSkippedDir(const QString& relPath) {
    const auto segs = relPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& s : segs)
        if (skipDirs().contains(s)) return true;
    return false;
}

// A NUL byte in the first chunk means it isn't text. Dumping it would flood the
// model's context with garbage and can carry invalid UTF-8.
bool looksBinary(const QByteArray& head) { return head.contains('\0'); }

// Translate a glob to a PCRE. Qt's wildcardToRegularExpression has no "**"
// (recursive) support and its behaviour around path separators changed across
// 6.x, so we own the translation: ** crosses directories, * and ? do not.
QRegularExpression globToRegex(const QString& pattern) {
    QString rx = QStringLiteral("^");
    for (int i = 0; i < pattern.size(); ++i) {
        const QChar c = pattern.at(i);
        if (c == QLatin1Char('*')) {
            const bool dbl = (i + 1 < pattern.size() && pattern.at(i + 1) == QLatin1Char('*'));
            if (dbl) {
                ++i;
                if (i + 1 < pattern.size() && pattern.at(i + 1) == QLatin1Char('/')) {
                    ++i;
                    rx += QStringLiteral("(?:.*/)?");  // **/ also matches zero directories
                } else {
                    rx += QStringLiteral(".*");
                }
            } else {
                rx += QStringLiteral("[^/]*");
            }
        } else if (c == QLatin1Char('?')) {
            rx += QStringLiteral("[^/]");
        } else if (c == QLatin1Char('{')) {
            rx += QStringLiteral("(?:");
        } else if (c == QLatin1Char('}')) {
            rx += QLatin1Char(')');
        } else if (c == QLatin1Char(',')) {
            rx += QLatin1Char('|');  // only meaningful inside {...}; harmless elsewhere
        } else {
            rx += QRegularExpression::escape(QString(c));
        }
    }
    rx += QLatin1Char('$');
    return QRegularExpression(rx);
}

// --------------------------------------------------------------- shell runner

struct ShellOut {
    QString text;
    int exitCode = 0;
    bool timedOut = false;
    bool truncated = false;
};

// Run a command with a HARD TIMEOUT and an OUTPUT CAP. A hanging command (a
// server, `sleep`, an infinite loop) must not wedge the agent, and a runaway one
// (`yes`, `cat hugefile`) must not flood memory or the model's context. We keep
// draining after the cap is hit so the child never deadlocks on a full pipe.
ShellOut runShell(const QString& cmd, const QString& workDir, int timeoutSec = -1,
                  int maxBytes = -1) {
    if (timeoutSec < 0) timeoutSec = qMax(5, Config::integer(QStringLiteral("agents.bashTimeout"), 120));
    if (maxBytes < 0) maxBytes = qMax(4096, Config::integer(QStringLiteral("agents.bashMaxBytes"), 200000));

    ShellOut r;
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    if (!workDir.isEmpty()) p.setWorkingDirectory(workDir);
#ifdef Q_OS_WIN
    p.start(QStringLiteral("cmd"), {QStringLiteral("/C"), cmd});
#else
    p.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), cmd});
#endif
    if (!p.waitForStarted(5000)) {
        r.exitCode = -1;
        r.text = QStringLiteral("failed to start shell");
        return r;
    }

    QElapsedTimer clock;
    clock.start();
    QByteArray out;
    while (p.state() != QProcess::NotRunning) {
        p.waitForReadyRead(100);
        const QByteArray chunk = p.readAll();
        if (!chunk.isEmpty()) {
            const int remain = maxBytes - out.size();
            if (remain > 0) out.append(chunk.left(remain));
            if (out.size() >= maxBytes) r.truncated = true;  // keep draining, discard the rest
        }
        if (clock.elapsed() > qint64(timeoutSec) * 1000) {
            r.timedOut = true;
            p.kill();
            p.waitForFinished(2000);
            break;
        }
    }
    p.waitForFinished(1000);
    const QByteArray tail = p.readAll();
    if (!tail.isEmpty()) {
        const int remain = maxBytes - out.size();
        if (remain > 0) out.append(tail.left(remain));
        if (out.size() >= maxBytes) r.truncated = true;
    }

    r.exitCode = p.exitCode();
    r.text = QString::fromUtf8(out);
    if (r.timedOut)
        r.text += QStringLiteral("\n…[command killed after %1s — it ran too long]").arg(timeoutSec);
    else if (r.truncated)
        r.text += QStringLiteral("\n…[output truncated at %1 bytes]").arg(maxBytes);
    if (r.text.trimmed().isEmpty()) r.text = QStringLiteral("(no output)");
    return r;
}

// ------------------------------------------------------------------ registry

struct Registry {
    QVector<ToolDef> defs;      // registration order == schema order
    QHash<QString, int> index;  // name -> position in defs
    bool populated = false;
    QMutex mutex;
};

Registry& registry() {
    static Registry r;
    return r;
}

// The thread-local sandbox root. See Tools::resolvePath.
thread_local QString g_threadRoot;

// --------------------------------------------------------- schema shorthands

QJsonObject prop(const QString& type, const QString& desc) {
    return QJsonObject{{"type", type}, {"description", desc}};
}
QJsonObject strProp(const QString& d) { return prop(QStringLiteral("string"), d); }
QJsonObject intProp(const QString& d) { return prop(QStringLiteral("integer"), d); }
QJsonObject boolProp(const QString& d) { return prop(QStringLiteral("boolean"), d); }
QJsonObject arrProp(const QString& d) {
    return QJsonObject{{"type", "array"},
                       {"description", d},
                       {"items", QJsonObject{{"type", "object"}}}};
}

// `props` is a QJsonObject even when empty — see the QUIRK note in Tools.h.
QJsonObject params(const QJsonObject& props, const QStringList& required) {
    QJsonArray req;
    for (const QString& r : required) req.append(r);
    return QJsonObject{{"type", "object"}, {"properties", props}, {"required", req}};
}

}  // namespace

// ============================================================== Permission

namespace {
struct PermState {
    std::atomic<int> mode{static_cast<int>(PermMode::Ask)};
    std::atomic<int> planReturn{static_cast<int>(PermMode::Ask)};
    std::atomic<bool> interactive{false};
    QMutex mutex;  // guards the callbacks only
    std::function<bool(const ToolDef&, const QJsonObject&)> asker;
    std::function<bool(const QString&)> planApprover;
};
PermState& perm() {
    static PermState s;
    return s;
}
}  // namespace

void Permission::setMode(PermMode m) {
    PermState& s = perm();
    const PermMode cur = static_cast<PermMode>(s.mode.load());
    // Entering plan mode remembers the mode to come back to on approval.
    if (m == PermMode::Plan && cur != PermMode::Plan) s.planReturn.store(static_cast<int>(cur));
    s.mode.store(static_cast<int>(m));
}

PermMode Permission::mode() { return static_cast<PermMode>(perm().mode.load()); }
void Permission::setInteractive(bool on) { perm().interactive.store(on); }
bool Permission::interactive() { return perm().interactive.load(); }

void Permission::setAsker(std::function<bool(const ToolDef&, const QJsonObject&)> fn) {
    PermState& s = perm();
    QMutexLocker lock(&s.mutex);
    s.asker = std::move(fn);
}

void Permission::setPlanApprover(std::function<bool(const QString&)> fn) {
    PermState& s = perm();
    QMutexLocker lock(&s.mutex);
    s.planApprover = std::move(fn);
}

bool Permission::approvePlan(const QString& plan) {
    PermState& s = perm();
    std::function<bool(const QString&)> fn;
    {
        QMutexLocker lock(&s.mutex);
        fn = s.planApprover;
    }
    // No approver installed => a non-interactive run (crew/subagent/one-shot).
    // The model must not be able to talk its own way out of plan mode.
    if (!fn || !s.interactive.load()) return false;
    return fn(plan);
}

PermMode Permission::exitPlan() {
    PermState& s = perm();
    const PermMode back = static_cast<PermMode>(s.planReturn.load());
    s.mode.store(static_cast<int>(back));
    return back;
}

bool Permission::check(const ToolDef& t, const QJsonObject& args) {
    if (!t.mutates) return true;  // read-only is always safe

    PermState& s = perm();
    switch (static_cast<PermMode>(s.mode.load())) {
        case PermMode::Auto:
            return true;
        case PermMode::ReadOnly:
        case PermMode::Plan:
            return false;
        case PermMode::Ask:
            break;
    }
    // Ask: with nobody at the keyboard there is no one to approve, so a mutation
    // is denied rather than waved through.
    if (!s.interactive.load()) return false;
    std::function<bool(const ToolDef&, const QJsonObject&)> fn;
    {
        QMutexLocker lock(&s.mutex);
        fn = s.asker;
    }
    if (!fn) return false;
    return fn(t, args);
}

QString Permission::modeName(PermMode m) {
    switch (m) {
        case PermMode::Auto: return QStringLiteral("auto");
        case PermMode::Ask: return QStringLiteral("ask");
        case PermMode::ReadOnly: return QStringLiteral("readonly");
        case PermMode::Plan: return QStringLiteral("plan");
    }
    return QStringLiteral("ask");
}

PermMode Permission::modeFromName(const QString& s) {
    const QString v = s.trimmed().toLower();
    if (v == QLatin1String("auto")) return PermMode::Auto;
    if (v == QLatin1String("readonly")) return PermMode::ReadOnly;
    if (v == QLatin1String("plan")) return PermMode::Plan;
    return PermMode::Ask;
}

// ==================================================================== Tools

void Tools::setThreadRoot(const QString& root) {
    g_threadRoot = root.isEmpty() ? QString() : QFileInfo(root).absoluteFilePath();
}

QString Tools::threadRoot() {
    return g_threadRoot.isEmpty() ? QDir::currentPath() : g_threadRoot;
}

bool Tools::hasThreadRoot() { return !g_threadRoot.isEmpty(); }

namespace {
QMutex g_askerMutex;
std::function<QString(const QString&, const QStringList&)> g_asker;
}  // namespace

void Tools::setQuestionAsker(std::function<QString(const QString&, const QStringList&)> fn) {
    QMutexLocker lock(&g_askerMutex);
    g_asker = std::move(fn);
}

QString Tools::askQuestion(const QString& question, const QStringList& options, bool* asked) {
    if (asked) *asked = false;
    // No asker (a headless run), or a run with nobody watching (a crew coder on a
    // worker thread): there is no human to answer, so we do NOT block. The caller
    // turns this into "proceed on a stated assumption".
    if (!Permission::interactive()) return {};
    std::function<QString(const QString&, const QStringList&)> fn;
    {
        QMutexLocker lock(&g_askerMutex);
        fn = g_asker;
    }
    if (!fn) return {};
    if (asked) *asked = true;
    return fn(question, options);
}

QString Tools::resolvePath(const QString& p, bool* ok) {
    if (ok) *ok = true;
    QString in = p.trimmed();
    if (in.isEmpty()) in = QStringLiteral(".");

    // ~ expansion, which models emit constantly.
    if (in == QLatin1String("~"))
        in = QDir::homePath();
    else if (in.startsWith(QLatin1String("~/")))
        in = QDir::homePath() + in.mid(1);

    const QString root = threadRoot();
    QString abs = QDir::isAbsolutePath(in) ? in : root + QLatin1Char('/') + in;
    // cleanPath collapses ".." lexically — the check below therefore sees the real
    // target even for a/../../etc/passwd, which never touches the filesystem.
    abs = QDir::cleanPath(abs);

    // Confinement only applies to an EXPLICIT thread root (a crew sandbox). In a
    // plain CLI session the user's cwd is not a jail and absolute paths elsewhere
    // are legitimate.
    if (!g_threadRoot.isEmpty()) {
        const QString jail = QDir::cleanPath(g_threadRoot);
        if (abs != jail && !abs.startsWith(jail + QLatin1Char('/'))) {
            if (ok) *ok = false;
            return QString();
        }
    }
    return abs;
}

namespace {

// Resolve or bail. Used by every filesystem tool so an escape attempt becomes a
// tool error the model can read, not a write outside the sandbox.
bool resolveOr(const QString& raw, QString* out, ToolResult* err) {
    bool ok = false;
    const QString abs = Tools::resolvePath(raw, &ok);
    if (!ok) {
        *err = fail(QStringLiteral("path '%1' is outside the working root (%2) — refused")
                        .arg(raw, Tools::threadRoot()));
        return false;
    }
    *out = abs;
    return true;
}

void add(Registry& r, ToolDef d) {
    r.index.insert(d.name, r.defs.size());
    r.defs.push_back(std::move(d));
}

// ------------------------------------------------------------- tool bodies

ToolResult toolView(const QJsonObject& a) {
    const QString raw = argStr(a, {"file_path", "file", "path"});
    if (raw.isEmpty()) return fail(QStringLiteral("missing required parameter 'file_path'"));
    QString path;
    ToolResult err;
    if (!resolveOr(raw, &path, &err)) return err;

    QFileInfo fi(path);
    if (!fi.exists()) return fail(QStringLiteral("file not found: %1").arg(path));
    if (fi.isDir()) return fail(QStringLiteral("path is a directory — use ls: %1").arg(path));

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("unable to read (permissions?): %1").arg(path));
    const QByteArray head = f.peek(8192);
    if (looksBinary(head)) {
        f.close();
        return okay(QStringLiteral("Binary file (%1 bytes) — not shown as text: %2")
                        .arg(fi.size())
                        .arg(path));
    }

    const int offset = qMax(0, argInt(a, {"offset"}, 0));
    const int limit = qMax(1, argInt(a, {"limit"}, 2000));  // default cap, never the whole file

    QString out;
    int i = 0, shown = 0;
    bool more = false;
    while (!f.atEnd()) {
        const QByteArray lineBytes = f.readLine();
        if (i >= offset) {
            if (shown >= limit) {
                more = true;
                break;
            }
            QString line = QString::fromUtf8(lineBytes);
            if (!line.endsWith(QLatin1Char('\n'))) line += QLatin1Char('\n');
            out += QStringLiteral("%1  %2").arg(i + 1, 4).arg(line);
            ++shown;
        }
        ++i;
    }
    f.close();
    if (shown == 0)
        return okay(offset > 0
                        ? QStringLiteral("(no lines at offset %1 — the file has fewer lines)").arg(offset)
                        : QStringLiteral("(empty file)"));
    if (more)
        out += QStringLiteral("… [showing %1 lines starting at %2 — pass a larger offset/limit for more]\n")
                   .arg(shown)
                   .arg(offset + 1);
    return okay(out);
}

ToolResult toolWrite(const QJsonObject& a) {
    const QString raw = argStr(a, {"file_path", "path", "file"});
    if (raw.isEmpty()) return fail(QStringLiteral("missing required parameter 'file_path'"));
    if (!a.contains(QStringLiteral("content")))
        return fail(QStringLiteral("missing required parameter 'content'"));
    QString path;
    ToolResult err;
    if (!resolveOr(raw, &path, &err)) return err;

    const QString content = unfence(argStr(a, {"content"}));
    const QString dir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dir)) return fail(QStringLiteral("cannot create directory: %1").arg(dir));

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("error writing file: %1").arg(path));
    const QByteArray bytes = content.toUtf8();
    const bool wrote = f.write(bytes) == bytes.size();
    f.close();
    if (!wrote) return fail(QStringLiteral("short write: %1").arg(path));
    return okay(QStringLiteral("Wrote %1 (%2 bytes)").arg(path).arg(bytes.size()));
}

// Shared by edit/multi_edit: locate `old` exactly once, else fall back to the
// whitespace-flexible match. Returns false with `why` set when it can't.
bool locate(const QString& content, const QString& old, int* pos, int* len, QString* why) {
    const int count = countOccurrences(content, old);
    if (count > 1) {
        *why = QStringLiteral("old_string appears %1 times — include more surrounding lines so it "
                              "matches exactly one place")
                   .arg(count);
        return false;
    }
    if (count == 1) {
        *pos = content.indexOf(old);
        *len = old.size();
        return true;
    }
    if (fuzzyFind(content, old, pos, len)) return true;
    *why = QStringLiteral("old_string not found in file");
    return false;
}

ToolResult toolEdit(const QJsonObject& a) {
    const QString raw = argStr(a, {"file_path", "path", "file"});
    const QString oldStr = argStr(a, {"old_string"});
    const QString newStr = argStr(a, {"new_string"});
    if (raw.isEmpty()) return fail(QStringLiteral("missing required parameter 'file_path'"));
    if (oldStr.isEmpty()) return fail(QStringLiteral("missing required parameter 'old_string'"));
    QString path;
    ToolResult err;
    if (!resolveOr(raw, &path, &err)) return err;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("error reading file: %1").arg(path));
    const QString content = QString::fromUtf8(f.readAll());
    f.close();

    int pos = 0, len = 0;
    QString why;
    if (!locate(content, oldStr, &pos, &len, &why))
        return fail(QStringLiteral("%1 (%2)").arg(why, path));

    QString updated = content;
    updated.replace(pos, len, newStr);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("error writing file: %1").arg(path));
    f.write(updated.toUtf8());
    f.close();
    return okay(QStringLiteral("Edited %1").arg(path));
}

// All edits apply or none do: we mutate an in-memory copy and only write once
// every edit has landed. A half-applied multi_edit is worse than a rejected one.
ToolResult toolMultiEdit(const QJsonObject& a) {
    const QString raw = argStr(a, {"file_path", "path", "file"});
    if (raw.isEmpty()) return fail(QStringLiteral("missing required parameter 'file_path'"));
    const QJsonArray edits = a.value(QStringLiteral("edits")).toArray();
    if (edits.isEmpty())
        return fail(QStringLiteral("missing 'edits' (array of {old_string, new_string, [replace_all]})"));

    QString path;
    ToolResult err;
    if (!resolveOr(raw, &path, &err)) return err;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("error reading file: %1").arg(path));
    QString content = QString::fromUtf8(f.readAll());
    f.close();

    int applied = 0;
    for (int i = 0; i < edits.size(); ++i) {
        const QJsonObject e = edits.at(i).toObject();
        if (e.isEmpty())
            return fail(QStringLiteral("edit #%1: not an object (no edits applied)").arg(i + 1));
        const QString old = e.value(QStringLiteral("old_string")).toString();
        const QString neu = e.value(QStringLiteral("new_string")).toString();
        if (old.isEmpty())
            return fail(QStringLiteral("edit #%1: missing old_string (no edits applied)").arg(i + 1));

        if (argBool(e, {"replace_all"})) {
            const int n = countOccurrences(content, old);
            if (n == 0)
                return fail(QStringLiteral("edit #%1: old_string not found (no edits applied)").arg(i + 1));
            content.replace(old, neu);
            applied += n;
            continue;
        }
        int pos = 0, len = 0;
        QString why;
        if (!locate(content, old, &pos, &len, &why))
            return fail(QStringLiteral("edit #%1: %2 (no edits applied)").arg(i + 1).arg(why));
        content.replace(pos, len, neu);
        ++applied;
    }

    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("error writing file: %1").arg(path));
    f.write(content.toUtf8());
    f.close();
    return okay(QStringLiteral("Applied %1 edit(s) to %2").arg(applied).arg(path));
}

ToolResult toolMkdir(const QJsonObject& a) {
    const QString raw = argStr(a, {"path", "dir"});
    if (raw.isEmpty()) return fail(QStringLiteral("missing required parameter 'path'"));
    QString path;
    ToolResult err;
    if (!resolveOr(raw, &path, &err)) return err;
    if (QFileInfo(path).isDir()) return okay(QStringLiteral("Already exists: %1").arg(path));
    const bool parents = argBool(a, {"parents"});
    const bool made = parents ? QDir().mkpath(path) : QDir().mkdir(path);
    return made ? okay(QStringLiteral("Created: %1").arg(path))
                : fail(QStringLiteral("failed to create: %1 (use parents=true if the parent is missing)")
                           .arg(path));
}

ToolResult toolTouch(const QJsonObject& a) {
    const QString raw = argStr(a, {"path", "file_path"});
    if (raw.isEmpty()) return fail(QStringLiteral("missing required parameter 'path'"));
    QString path;
    ToolResult err;
    if (!resolveOr(raw, &path, &err)) return err;

    if (QFileInfo::exists(path)) {
        QFile f(path);
        // Qt has no utime(); reopening for append and touching the size is the
        // portable way to bump mtime without altering content.
        if (!f.open(QIODevice::ReadWrite)) return fail(QStringLiteral("cannot touch: %1").arg(path));
        f.resize(f.size());
        f.close();
        return okay(QStringLiteral("Updated timestamp: %1").arg(path));
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return fail(QStringLiteral("failed to create: %1").arg(path));
    f.close();
    return okay(QStringLiteral("Created: %1").arg(path));
}

bool copyTree(const QString& src, const QString& dst, QString* err) {
    QFileInfo fi(src);
    if (fi.isDir()) {
        if (!QDir().mkpath(dst)) {
            *err = QStringLiteral("cannot create %1").arg(dst);
            return false;
        }
        QDirIterator it(src, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
        while (it.hasNext()) {
            it.next();
            if (!copyTree(it.filePath(), dst + QLatin1Char('/') + it.fileName(), err)) return false;
        }
        return true;
    }
    QDir().mkpath(QFileInfo(dst).absolutePath());
    QFile::remove(dst);  // QFile::copy refuses to overwrite
    if (!QFile::copy(src, dst)) {
        *err = QStringLiteral("copy failed: %1 -> %2").arg(src, dst);
        return false;
    }
    return true;
}

ToolResult toolCp(const QJsonObject& a) {
    const QString rawSrc = argStr(a, {"src", "source"});
    const QString rawDst = argStr(a, {"dst", "dest", "destination"});
    if (rawSrc.isEmpty() || rawDst.isEmpty()) return fail(QStringLiteral("missing 'src' or 'dst'"));
    QString src, dst;
    ToolResult err;
    if (!resolveOr(rawSrc, &src, &err)) return err;
    if (!resolveOr(rawDst, &dst, &err)) return err;
    if (!QFileInfo::exists(src)) return fail(QStringLiteral("source not found: %1").arg(src));

    // `cp a b/` where b is an existing dir means "into b", as with real cp.
    QString target = dst;
    if (QFileInfo(dst).isDir() && !QFileInfo(src).isDir())
        target = dst + QLatin1Char('/') + QFileInfo(src).fileName();

    QString why;
    if (!copyTree(src, target, &why)) return fail(why);
    return okay(QStringLiteral("Copied %1 -> %2").arg(src, target));
}

ToolResult toolMv(const QJsonObject& a) {
    const QString rawSrc = argStr(a, {"src", "source"});
    const QString rawDst = argStr(a, {"dst", "dest", "destination"});
    if (rawSrc.isEmpty() || rawDst.isEmpty()) return fail(QStringLiteral("missing 'src' or 'dst'"));
    QString src, dst;
    ToolResult err;
    if (!resolveOr(rawSrc, &src, &err)) return err;
    if (!resolveOr(rawDst, &dst, &err)) return err;
    if (!QFileInfo::exists(src)) return fail(QStringLiteral("source not found: %1").arg(src));

    QString target = dst;
    if (QFileInfo(dst).isDir() && !QFileInfo(src).isDir())
        target = dst + QLatin1Char('/') + QFileInfo(src).fileName();
    QDir().mkpath(QFileInfo(target).absolutePath());

    if (QFile::rename(src, target)) return okay(QStringLiteral("Moved %1 -> %2").arg(src, target));
    // rename() fails across filesystems; fall back to copy + delete.
    QString why;
    if (!copyTree(src, target, &why)) return fail(why);
    const bool gone = QFileInfo(src).isDir() ? QDir(src).removeRecursively() : QFile::remove(src);
    if (!gone) return fail(QStringLiteral("copied to %1 but could not remove %2").arg(target, src));
    return okay(QStringLiteral("Moved %1 -> %2").arg(src, target));
}

ToolResult toolRm(const QJsonObject& a) {
    const QString raw = argStr(a, {"path", "file_path"});
    if (raw.isEmpty()) return fail(QStringLiteral("missing required parameter 'path'"));
    const bool recursive = argBool(a, {"recursive", "r"});
    const bool dryRun = argBool(a, {"dry_run", "dry-run"});
    QString path;
    ToolResult err;
    if (!resolveOr(raw, &path, &err)) return err;

    // Never let the model nuke a VCS or dependency tree — the two "delete this and
    // the project is unrecoverable" directories.
    if (path.contains(QLatin1String("node_modules")) || path.contains(QLatin1String("/.git")))
        return fail(QStringLiteral("refusing to remove a system directory (node_modules/.git): %1").arg(path));
    if (!QFileInfo::exists(path)) return fail(QStringLiteral("path not found: %1").arg(path));

    const bool isDir = QFileInfo(path).isDir();
    if (dryRun)
        return okay(QStringLiteral("[DRY RUN] would remove %1%2")
                        .arg(path, isDir ? QStringLiteral(" (directory)") : QString()));
    if (isDir) {
        if (!recursive) return fail(QStringLiteral("%1 is a directory — pass recursive=true").arg(path));
        return QDir(path).removeRecursively() ? okay(QStringLiteral("Removed: %1").arg(path))
                                              : fail(QStringLiteral("failed to remove: %1").arg(path));
    }
    return QFile::remove(path) ? okay(QStringLiteral("Removed: %1").arg(path))
                               : fail(QStringLiteral("failed to remove: %1").arg(path));
}

ToolResult toolPatch(const QJsonObject& a) {
    const QString raw = argStr(a, {"file_path", "path", "file"});
    const QString diff = argStr(a, {"diff", "patch"});
    if (raw.isEmpty()) return fail(QStringLiteral("missing required parameter 'file_path'"));
    if (diff.isEmpty()) return fail(QStringLiteral("missing required parameter 'diff'"));
    QString path;
    ToolResult err;
    if (!resolveOr(raw, &path, &err)) return err;
    if (!QFileInfo::exists(path)) return fail(QStringLiteral("file not found: %1").arg(path));

    QTemporaryFile tmp(QDir::tempPath() + QStringLiteral("/odv-patch-XXXXXX.diff"));
    if (!tmp.open()) return fail(QStringLiteral("cannot create temp file for the patch"));
    QString body = diff;
    if (!body.endsWith(QLatin1Char('\n'))) body += QLatin1Char('\n');  // patch(1) rejects a truncated last hunk
    tmp.write(body.toUtf8());
    tmp.flush();

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.setWorkingDirectory(Tools::threadRoot());
    // -p1 strips the leading a/ b/ component of a git-style diff; --forward stops
    // patch from "un-applying" a diff it thinks is already applied (which silently
    // reverts the model's own work).
    p.start(QStringLiteral("patch"),
            {QStringLiteral("-p1"), QStringLiteral("--forward"), QStringLiteral("-i"),
             tmp.fileName()});
    if (!p.waitForStarted(5000)) return fail(QStringLiteral("`patch` is not available on PATH"));
    if (!p.waitForFinished(60000)) {
        p.kill();
        p.waitForFinished(2000);
        return fail(QStringLiteral("patch timed out"));
    }
    const QString out = QString::fromUtf8(p.readAll()).trimmed();
    if (p.exitCode() != 0)
        return fail(QStringLiteral("patch failed (exit %1): %2").arg(p.exitCode()).arg(out));
    return okay(out.isEmpty() ? QStringLiteral("Patched %1").arg(path) : out);
}

ToolResult toolLs(const QJsonObject& a) {
    QString path;
    ToolResult err;
    if (!resolveOr(argStr(a, {"path", "dir", "file_path"}, QStringLiteral(".")), &path, &err)) return err;
    QFileInfo fi(path);
    if (!fi.isDir()) return fail(QStringLiteral("not a directory: %1").arg(path));

    QDir dir(path);
    const auto entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                                           QDir::DirsFirst | QDir::Name);
    if (entries.isEmpty()) return okay(QStringLiteral("Empty directory"));
    QString out;
    for (const QFileInfo& e : entries) {
        out += QStringLiteral("%1 %2 %3 %4\n")
                   .arg(e.isDir() ? QLatin1Char('d') : QLatin1Char('-'))
                   .arg(e.size(), 9)
                   .arg(e.lastModified().toString(QStringLiteral("MMM dd HH:mm")))
                   .arg(e.fileName() + (e.isDir() ? QStringLiteral("/") : QString()));
    }
    return okay(out);
}

ToolResult toolGrep(const QJsonObject& a) {
    const QString pattern = argStr(a, {"pattern", "query", "regex"});
    if (pattern.isEmpty()) return fail(QStringLiteral("missing required parameter 'pattern'"));
    const QString include = argStr(a, {"include", "glob"});

    QString root;
    ToolResult err;
    if (!resolveOr(argStr(a, {"path", "dir"}, QStringLiteral(".")), &root, &err)) return err;

    const QRegularExpression rx(pattern);
    if (!rx.isValid())
        return fail(QStringLiteral("invalid regex: %1").arg(rx.errorString()));
    const QRegularExpression inc = include.isEmpty() ? QRegularExpression() : globToRegex(include);

    // Native walk rather than shelling out to grep(1): QRegularExpression IS PCRE2,
    // so we get the same semantics with no process, no quoting bugs, and no
    // dependency on the host having GNU grep.
    QStringList files;
    QFileInfo rootInfo(root);
    if (rootInfo.isFile()) {
        files << root;
    } else if (rootInfo.isDir()) {
        QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString f = it.next();
            const QString rel = QDir(root).relativeFilePath(f);
            if (underSkippedDir(rel)) continue;
            if (!include.isEmpty() && !inc.match(QFileInfo(f).fileName()).hasMatch() &&
                !inc.match(rel).hasMatch())
                continue;
            files << f;
        }
    } else {
        return fail(QStringLiteral("path not found: %1").arg(root));
    }

    const int kMaxHits = 500;
    QString out;
    int hits = 0;
    for (const QString& f : files) {
        if (hits >= kMaxHits) break;
        QFile fh(f);
        if (!fh.open(QIODevice::ReadOnly)) continue;
        if (looksBinary(fh.peek(8192))) {
            fh.close();
            continue;
        }
        int lineNo = 0;
        while (!fh.atEnd() && hits < kMaxHits) {
            ++lineNo;
            QString line = QString::fromUtf8(fh.readLine()).trimmed();
            if (!rx.match(line).hasMatch()) continue;
            if (line.size() > 300) line = line.left(300) + QStringLiteral("…");
            out += QStringLiteral("%1:%2: %3\n").arg(f).arg(lineNo).arg(line);
            ++hits;
        }
        fh.close();
    }
    if (hits == 0) return okay(QStringLiteral("No matches found"));
    if (hits >= kMaxHits)
        out += QStringLiteral("…[stopped at %1 matches — narrow the pattern or path]\n").arg(kMaxHits);
    return okay(out);
}

ToolResult toolGlob(const QJsonObject& a) {
    QString pattern = argStr(a, {"pattern", "glob"});
    if (pattern.isEmpty()) return fail(QStringLiteral("missing required parameter 'pattern'"));
    QString base;
    ToolResult err;
    if (!resolveOr(argStr(a, {"path", "dir"}, QStringLiteral(".")), &base, &err)) return err;
    if (!QFileInfo(base).isDir()) return fail(QStringLiteral("not a directory: %1").arg(base));

    // A bare "*.cpp" means "anywhere in the tree" to every model that emits it.
    if (!pattern.contains(QLatin1Char('/'))) pattern = QStringLiteral("**/") + pattern;
    const QRegularExpression rx = globToRegex(pattern);
    if (!rx.isValid()) return fail(QStringLiteral("invalid glob: %1").arg(pattern));

    const int kMax = 500;
    QStringList hits;
    QDir baseDir(base);
    QDirIterator it(base, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
                    QDirIterator::Subdirectories);
    int total = 0;
    while (it.hasNext()) {
        const QString f = it.next();
        const QString rel = baseDir.relativeFilePath(f);
        if (underSkippedDir(rel)) continue;
        if (!rx.match(rel).hasMatch()) continue;
        ++total;
        if (hits.size() < kMax) hits << f;
    }
    if (hits.isEmpty()) return okay(QStringLiteral("No files found"));
    hits.sort();
    QString out = hits.join(QLatin1Char('\n'));
    if (total > kMax)
        out += QStringLiteral("\n…[%1 matches; showing %2 — narrow the pattern]").arg(total).arg(kMax);
    return okay(out);
}

ToolResult toolBash(const QJsonObject& a) {
    QString cmd = argStr(a, {"command", "cmd"}).trimmed();
    if (cmd.isEmpty()) return fail(QStringLiteral("missing required parameter 'command'"));

    // Commands that are unrecoverable no matter which mode we are in: a sandbox
    // protects the project tree, not the user's machine.
    static const QStringList kBanned{QStringLiteral("rm -rf /"),   QStringLiteral("rm -rf ~"),
                                     QStringLiteral("rm -rf $HOME"), QStringLiteral("mkfs"),
                                     QStringLiteral(":(){"),        QStringLiteral("sudo rm"),
                                     QStringLiteral("> /dev/sd"),   QStringLiteral("dd if=")};
    for (const QString& b : kBanned)
        if (cmd.contains(b)) return fail(QStringLiteral("dangerous command blocked: %1").arg(b));

    // Working directory is the THREAD's root, never the process cwd — parallel
    // crew coders each shell out inside their own sandbox.
    const ShellOut r = runShell(cmd, Tools::threadRoot());
    QString out = r.text;
    if (r.exitCode != 0 && !r.timedOut)
        out += QStringLiteral("\n[exit status %1]").arg(r.exitCode);
    return okay(out);
}

ToolResult toolTodoWrite(const QJsonObject& a) {
    const QJsonArray todos = a.value(QStringLiteral("todos")).isArray()
                                 ? a.value(QStringLiteral("todos")).toArray()
                                 : a.value(QStringLiteral("items")).toArray();
    QJsonArray norm;
    for (const QJsonValue& v : todos) {
        const QJsonObject t = v.toObject();
        const QString content = argStr(t, {"content", "task", "text"}).trimmed();
        if (content.isEmpty()) continue;
        QString status = argStr(t, {"status"}, QStringLiteral("pending"));
        if (status != QLatin1String("pending") && status != QLatin1String("in_progress") &&
            status != QLatin1String("completed"))
            status = QStringLiteral("pending");
        norm.append(QJsonObject{{"content", content},
                                {"status", status},
                                {"activeForm", argStr(t, {"activeForm"})}});
    }

    const QString dir = Config::dataDir() + QStringLiteral("/todos");
    QDir().mkpath(dir);
    QFile f(dir + QStringLiteral("/current.json"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(norm).toJson(QJsonDocument::Compact));
        f.close();
    }
    if (norm.isEmpty()) return okay(QStringLiteral("Todo list cleared."));

    int done = 0;
    for (const QJsonValue& v : norm)
        if (v.toObject().value(QStringLiteral("status")).toString() == QLatin1String("completed")) ++done;
    QString out = QStringLiteral("Todo list (%1/%2 done):\n").arg(done).arg(norm.size());
    for (const QJsonValue& v : norm) {
        const QJsonObject t = v.toObject();
        const QString s = t.value(QStringLiteral("status")).toString();
        const QString mark = s == QLatin1String("completed")     ? QStringLiteral("[x]")
                             : s == QLatin1String("in_progress") ? QStringLiteral("[~]")
                                                                 : QStringLiteral("[ ]");
        out += QStringLiteral("  %1 %2\n").arg(mark, t.value(QStringLiteral("content")).toString());
    }
    return okay(out.trimmed());
}

ToolResult toolExitPlanMode(const QJsonObject& a) {
    const QString plan = argStr(a, {"plan", "steps"}).trimmed();
    if (Permission::mode() != PermMode::Plan)
        return okay(QStringLiteral("Not in plan mode — just proceed with the task."));
    if (plan.isEmpty())
        return fail(QStringLiteral("provide the plan: exit_plan_mode(plan: \"…\")"));

    // Approval is the USER's call. In a non-interactive run (crew, subagent,
    // one-shot) no approver is installed, so the model cannot self-exit plan mode
    // and mutations stay blocked.
    if (!Permission::approvePlan(plan))
        return okay(QStringLiteral(
            "Plan recorded but NOT approved — stay read-only, do not attempt edits. "
            "Incorporate any feedback and propose a revised plan."));
    const PermMode back = Permission::exitPlan();
    return okay(QStringLiteral("Plan approved — plan mode off (now: %1). Implement it now.")
                    .arg(Permission::modeName(back)));
}

// ------------------------------------------------------------ skills & memory

// The payoff of progressive disclosure: the system prompt carries only each
// skill's name+description, and THIS is how the model pulls a body — one skill,
// once, when it decides the skill applies.
ToolResult toolSkill(const QJsonObject& a) {
    const QString name = argStr(a, {"name", "skill"}).trimmed();
    const QStringList have = Skills::names();
    const QString available =
        have.isEmpty() ? QStringLiteral("(none)") : have.join(QStringLiteral(", "));

    if (name.isEmpty())
        return okay(QStringLiteral("Usage: skill(name). Available skills: %1").arg(available));

    const Skill s = Skills::get(name);
    if (s.isNull())
        return okay(QStringLiteral("No skill named '%1'. Available: %2").arg(name, available));

    QString out = QStringLiteral("# Skill: %1\n%2\n\n%3").arg(s.name, s.description, s.body);
    if (!s.files.isEmpty())
        out += QStringLiteral("\n\n(Helper files in %1: %2 — read them with the view tool if "
                              "needed.)")
                   .arg(s.dir, s.files.join(QStringLiteral(", ")));
    return okay(out);
}

ToolResult toolRecall(const QJsonObject& a) {
    const QString slug = argStr(a, {"slug"}).trimmed();
    const QString query = argStr(a, {"query"}).trimmed();

    if (!slug.isEmpty()) {
        const MemoryNote m = Memory::get(slug);
        if (m.isNull()) {
            const QString idx = Memory::index();
            return okay(QStringLiteral("No memory '%1'. %2")
                            .arg(slug, idx.isEmpty() ? QStringLiteral("Memory is empty.")
                                                     : QStringLiteral("Known:\n") + idx));
        }
        QString out = QStringLiteral("# %1  (%2)").arg(m.title, m.slug);
        if (!m.tags.isEmpty())
            out += QStringLiteral("  tags: %1").arg(m.tags.join(QStringLiteral(", ")));
        out += QLatin1Char('\n') + m.body.trimmed();
        if (!m.links.isEmpty()) {
            QStringList wiki;
            for (const QString& l : m.links) wiki << QStringLiteral("[[%1]]").arg(l);
            out += QStringLiteral("\nLinks: %1").arg(wiki.join(QStringLiteral(", ")));
        }
        return okay(out);
    }

    if (!query.isEmpty()) {
        const auto hits = Memory::search(query);
        if (hits.isEmpty())
            return okay(QStringLiteral("No memories match '%1'.").arg(query));
        QString out = QStringLiteral("Memories matching '%1':\n").arg(query);
        for (const MemoryNote& m : hits)
            out += QStringLiteral("- %1 — %2\n").arg(m.slug, m.title);
        return okay(out + QStringLiteral("Read one with recall(slug)."));
    }

    const QString idx = Memory::index();
    if (idx.isEmpty())
        return okay(QStringLiteral("Memory is empty. Save facts with the remember tool."));
    return okay(QStringLiteral("Project memory:\n%1\nRead one with recall(slug), or search with "
                               "recall(query).")
                    .arg(idx));
}

ToolResult toolRemember(const QJsonObject& a) {
    QString title = argStr(a, {"title"}).trimmed();
    const QString content = argStr(a, {"content", "body"});
    if (title.isEmpty() && content.trimmed().isEmpty())
        return fail(QStringLiteral("remember needs a title and content."));

    if (title.isEmpty()) {
        static const QRegularExpression ws(QStringLiteral("\\s+"));
        title = content.trimmed();
        title.replace(ws, QStringLiteral(" "));
        title = title.left(48);
    }

    // Models send tags either as a real array or as "a, b" — accept both.
    QStringList tags;
    const QJsonValue tv = a.value(QStringLiteral("tags"));
    if (tv.isArray()) {
        const auto arr = tv.toArray();
        for (const QJsonValue& v : arr) {
            const QString t = v.toString().trimmed();
            if (!t.isEmpty()) tags << t;
        }
    } else if (tv.isString()) {
        const auto parts =
            tv.toString().split(QRegularExpression(QStringLiteral("[,;]")), Qt::SkipEmptyParts);
        for (const QString& p : parts)
            if (!p.trimmed().isEmpty()) tags << p.trimmed();
    }

    const QString slug = Memory::save(title, content, tags, argStr(a, {"slug"}));
    return okay(QStringLiteral("Saved memory '%1' (\"%2\"). Link to it from other notes with "
                               "[[%1]].")
                    .arg(slug, title));
}

// ------------------------------------------------------------- network tools

// Both network tools answer to TWO gates: Permission (they are registered with
// mutates=true, so a read-only/plan session — and an MCP client, which defaults
// to read-only — cannot reach the network at all) and the web kill switch below
// (`--no-web`, config web.enabled). Egress is a side effect on the world even
// when nothing on disk changes.
ToolResult toolSearch(const QJsonObject& a) {
    const QString q = argStr(a, {"query", "q"}).trimmed();
    if (q.isEmpty()) return fail(QStringLiteral("missing required parameter 'query'"));
    if (!WebSearch::webEnabled())
        return fail(QStringLiteral("web access is off (--no-web / web.enabled=false) — "
                                   "search is unavailable in this run"));
    // A separate switch from web.enabled: it silences SEARCH while leaving fetch
    // usable, for a user who wants the agent to read URLs they name but not to go
    // looking for its own.
    if (!Config::boolean(QStringLiteral("search.enabled"), true))
        return fail(QStringLiteral("Web search is disabled (search.enabled is false). Enable it "
                                   "with: ollamadev config set search.enabled true"));

    const int limit = argInt(a, {"limit"}, 5);
    const SearchResult r = WebSearch::search(q, limit, argStr(a, {"provider"}));
    if (!r.ok)
        return fail(r.error.isEmpty() ? QStringLiteral("no results for: %1").arg(q)
                                      : QStringLiteral("search failed: %1").arg(r.error));

    QString out = QStringLiteral("Web search results for \"%1\" (%2):\n\n").arg(q, r.provider);
    int i = 0;
    for (const SearchHit& h : r.hits) {
        out += QStringLiteral("%1. %2\n   %3\n").arg(++i).arg(h.title, h.url);
        if (!h.snippet.isEmpty()) out += QStringLiteral("   %1\n").arg(h.snippet);
        out += QLatin1Char('\n');
    }
    return okay(out.trimmed());
}

ToolResult toolFetch(const QJsonObject& a) {
    const QString url = argStr(a, {"url"}).trimmed();
    if (url.isEmpty()) return fail(QStringLiteral("missing required parameter 'url'"));
    if (!WebSearch::webEnabled())
        return fail(QStringLiteral("web access is off (--no-web / web.enabled=false) — "
                                   "fetch is unavailable in this run"));

    const FetchedPage p = WebSearch::fetch(url, argInt(a, {"timeout"}, 30));
    if (!p.ok) return fail(QStringLiteral("failed to fetch %1: %2").arg(url, p.error));
    if (p.text.trimmed().isEmpty())
        return okay(QStringLiteral("(no readable text at %1 — HTTP %2)").arg(url).arg(p.status));
    return okay(p.text);
}

// ---------------------------------------------------------------- code_search

ToolResult toolCodeSearch(const QJsonObject& a) {
    const QString q = argStr(a, {"query", "q"}).trimmed();
    if (q.isEmpty()) return fail(QStringLiteral("missing required parameter 'query'"));
    const int limit = qBound(1, argInt(a, {"limit"}, 8), 20);

    const SearchReport r = CodeIndex::search(q, limit);
    if (!r.ok) {
        if (r.error == QLatin1String("no_index"))
            return fail(QStringLiteral("No semantic index yet. Build it first: ollamadev index build"));
        if (r.error == QLatin1String("embed_failed"))
            return fail(QStringLiteral("Embedding failed — is the model installed? Run: ollama pull %1")
                            .arg(CodeIndex::model()));
        return fail(QStringLiteral("code_search failed"));
    }
    if (r.hits.isEmpty()) return okay(QStringLiteral("No matches."));

    QString out = QStringLiteral("Semantically closest code for \"%1\":\n\n").arg(q);
    for (const IndexHit& h : r.hits) {
        out += QStringLiteral("%1:%2-%3  (score %4)\n")
                   .arg(h.file)
                   .arg(h.start)
                   .arg(h.end)
                   .arg(h.score, 0, 'f', 3);
        for (const QString& line : h.snippet.split(QLatin1Char('\n')))
            out += QStringLiteral("    %1\n").arg(line);
        out += QLatin1Char('\n');
    }
    return okay(out.trimmed());
}

// ------------------------------------------------------------------ run_tests

struct TestCmd {
    QString cmd;
    QString label;
};

// How does THIS project run its tests? `test.command` overrides everything.
//
// FLAGS: every flag below was checked against the tool's own --help before being
// written down (ctest --test-dir/--output-on-failure, pytest -q). Nothing here is
// recalled from memory — that is exactly how the PHP shipped dead flags for three
// different CLIs.
bool detectTests(TestCmd* out) {
    const QString override = Config::str(QStringLiteral("test.command")).trimmed();
    if (!override.isEmpty()) {
        *out = TestCmd{override, QStringLiteral("config")};
        return true;
    }

    const QString root = Tools::threadRoot();
    auto has = [&root](const char* rel) {
        return QFileInfo::exists(root + QLatin1Char('/') + QLatin1String(rel));
    };
    auto isDir = [&root](const char* rel) {
        return QFileInfo(root + QLatin1Char('/') + QLatin1String(rel)).isDir();
    };

    // A configured CMake build tree is the strongest signal, and it is this very
    // project's shape.
    if (has("CMakeLists.txt") && isDir("build")) {
        *out = TestCmd{QStringLiteral("ctest --test-dir build --output-on-failure"),
                       QStringLiteral("ctest")};
        return true;
    }
    if (has("phpunit.xml") || has("phpunit.xml.dist")) {
        *out = TestCmd{has("vendor/bin/phpunit") ? QStringLiteral("./vendor/bin/phpunit")
                                                 : QStringLiteral("phpunit"),
                       QStringLiteral("phpunit")};
        return true;
    }
    if (has("composer.json")) {
        QFile f(root + QStringLiteral("/composer.json"));
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject cj = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
            if (cj.value(QStringLiteral("scripts")).toObject().contains(QStringLiteral("test"))) {
                *out = TestCmd{QStringLiteral("composer test"), QStringLiteral("composer")};
                return true;
            }
        }
    }
    if (has("go.mod")) {
        *out = TestCmd{QStringLiteral("go test ./..."), QStringLiteral("go")};
        return true;
    }
    if (has("Cargo.toml")) {
        *out = TestCmd{QStringLiteral("cargo test"), QStringLiteral("cargo")};
        return true;
    }
    if (has("pytest.ini") || has("tox.ini") || has("pyproject.toml") || has("setup.cfg") ||
        isDir("tests")) {
        *out = TestCmd{QStringLiteral("pytest -q"), QStringLiteral("pytest")};
        return true;
    }
    if (has("Makefile") || has("makefile")) {
        QFile f(root + QStringLiteral("/") +
                (has("Makefile") ? QStringLiteral("Makefile") : QStringLiteral("makefile")));
        if (f.open(QIODevice::ReadOnly)) {
            const QString mk = QString::fromUtf8(f.readAll());
            f.close();
            static const QRegularExpression target(QStringLiteral("^test:"),
                                                   QRegularExpression::MultilineOption);
            if (target.match(mk).hasMatch()) {
                *out = TestCmd{QStringLiteral("make test"), QStringLiteral("make")};
                return true;
            }
        }
    }
    return false;
}

ToolResult toolRunTests(const QJsonObject&) {
    TestCmd t;
    if (!detectTests(&t))
        return fail(QStringLiteral("No test command detected for this project. Set test.command in "
                                   "config to enable."));

    const ShellOut r = runShell(t.cmd, Tools::threadRoot(),
                                Config::integer(QStringLiteral("test.timeout"), 600));

    // Only the tail matters: a failing suite's useful part is at the end, and a
    // full run can be tens of thousands of lines.
    QStringList lines = r.text.split(QLatin1Char('\n'));
    const bool clipped = lines.size() > 80;
    if (clipped) lines = lines.mid(lines.size() - 80);

    const QString head = r.exitCode == 0
                             ? QStringLiteral("TESTS PASSED")
                             : QStringLiteral("TESTS FAILED (exit %1)").arg(r.exitCode);
    return okay(QStringLiteral("%1  [%2]\n\n%3%4")
                    .arg(head, t.cmd,
                         clipped ? QStringLiteral("…(showing the last 80 lines)\n") : QString(),
                         lines.join(QLatin1Char('\n'))));
}

// ====================================================================== git
//
// Every git_* tool goes through GitFlow::git, which spawns git with an argv ARRAY
// inside Tools::threadRoot(), with a hard timeout, and hands back a real exit
// code. That is not incidental — it is the whole reason these are safe to give a
// model:
//   * argv, not a shell string, so a branch name or a ref can never BE syntax
//     (the PHP `git log` interpolated its `n` straight into a shell command);
//   * threadRoot, so a crew coder running `git checkout` cannot reach out of its
//     sandbox and move HEAD in your real repo;
//   * a real exit code, so a merge conflict reports as a FAILURE instead of
//     coming back looking like success with the conflict text as its "output".

ToolResult gitRun(const QStringList& args, const QString& stdinText = QString()) {
    if (!GitFlow::isRepo()) return fail(QStringLiteral("not a git repository"));
    const GitResult r = GitFlow::git(args, stdinText);
    const QString text = r.output.trimmed();
    if (!r.ok())
        return fail(text.isEmpty()
                        ? QStringLiteral("git %1 failed (exit %2)").arg(args.value(0)).arg(r.exit)
                        : text);
    return okay(text.isEmpty() ? QStringLiteral("(no output)") : text);
}

ToolResult toolGitStatus(const QJsonObject&) {
    return gitRun({QStringLiteral("status"), QStringLiteral("--short")});
}

ToolResult toolGitDiff(const QJsonObject& a) {
    QStringList args{QStringLiteral("--no-pager"), QStringLiteral("diff")};
    if (argBool(a, {"cached", "staged"})) args << QStringLiteral("--cached");
    const QString file = argStr(a, {"file", "path", "file_path"});
    // `--` first: a file named like a flag, or a ref and a path that share a name,
    // are both real and both resolve wrong without it.
    if (!file.isEmpty()) args << QStringLiteral("--") << file;
    return gitRun(args);
}

ToolResult toolGitLog(const QJsonObject& a) {
    const int n = qBound(1, argInt(a, {"n", "count", "limit"}, 10), 500);
    return gitRun({QStringLiteral("--no-pager"), QStringLiteral("log"), QStringLiteral("--oneline"),
                   QStringLiteral("-n"), QString::number(n)});
}

ToolResult toolGitBranch(const QJsonObject& a) {
    QStringList args{QStringLiteral("branch")};
    if (argBool(a, {"all"})) args << QStringLiteral("-a");
    return gitRun(args);
}

ToolResult toolGitShow(const QJsonObject& a) {
    QStringList args{QStringLiteral("--no-pager"), QStringLiteral("show")};
    if (argBool(a, {"stat"})) args << QStringLiteral("--stat");
    args << argStr(a, {"ref", "commit", "sha"}, QStringLiteral("HEAD"));
    return gitRun(args);
}

ToolResult toolGitRemote(const QJsonObject&) {
    return gitRun({QStringLiteral("remote"), QStringLiteral("-v")});
}

ToolResult toolGitFetch(const QJsonObject& a) {
    QStringList args{QStringLiteral("fetch")};
    if (argBool(a, {"all"})) args << QStringLiteral("--all");
    if (argBool(a, {"prune"})) args << QStringLiteral("--prune");
    return gitRun(args);
}

ToolResult toolGitCheckout(const QJsonObject& a) {
    const QString branch = argStr(a, {"branch", "name", "ref"});
    if (branch.isEmpty()) return fail(QStringLiteral("missing branch"));
    QStringList args{QStringLiteral("checkout")};
    if (argBool(a, {"new", "create", "b"})) args << QStringLiteral("-b");
    args << branch;
    return gitRun(args);
}

ToolResult toolGitAdd(const QJsonObject& a) {
    if (argBool(a, {"all"})) return gitRun({QStringLiteral("add"), QStringLiteral("-A")});
    QStringList args{QStringLiteral("add"), QStringLiteral("--")};
    const QJsonValue files = a.value(QLatin1String("files"));
    if (files.isArray()) {
        for (const auto& f : files.toArray())
            if (!f.toString().isEmpty()) args << f.toString();
    } else {
        args << argStr(a, {"files", "file", "path"}, QStringLiteral("."));
    }
    return gitRun(args);
}

ToolResult toolGitCommit(const QJsonObject& a) {
    const QString msg = argStr(a, {"message", "m", "msg"});
    if (msg.isEmpty()) return fail(QStringLiteral("missing commit message"));
    QStringList args{QStringLiteral("commit")};
    if (argBool(a, {"all", "a"})) args << QStringLiteral("-a");
    if (argBool(a, {"amend"})) args << QStringLiteral("--amend");
    // The message goes in over STDIN, not argv: a real commit message is
    // multi-line, and this one was written by a model reading a diff that may
    // itself have come from a repo you merely cloned.
    args << QStringLiteral("-F") << QStringLiteral("-");
    return gitRun(args, msg);
}

ToolResult toolGitMerge(const QJsonObject& a) {
    const QString branch = argStr(a, {"branch", "name", "ref"});
    if (branch.isEmpty()) return fail(QStringLiteral("missing branch"));
    QStringList args{QStringLiteral("merge")};
    if (argBool(a, {"no_ff", "noFf"})) args << QStringLiteral("--no-ff");
    if (argBool(a, {"squash"})) args << QStringLiteral("--squash");
    args << branch;
    return gitRun(args);
}

ToolResult toolGitRebase(const QJsonObject& a) {
    const QString branch = argStr(a, {"branch", "onto_branch", "upstream"});
    if (branch.isEmpty()) return fail(QStringLiteral("missing branch"));
    QStringList args{QStringLiteral("rebase")};
    const QString onto = argStr(a, {"onto"});
    if (!onto.isEmpty()) args << QStringLiteral("--onto") << onto;
    args << branch;
    return gitRun(args);
}

ToolResult toolGitStash(const QJsonObject& a) {
    if (argBool(a, {"list"})) return gitRun({QStringLiteral("stash"), QStringLiteral("list")});
    if (argBool(a, {"pop"})) return gitRun({QStringLiteral("stash"), QStringLiteral("pop")});
    if (argBool(a, {"drop"})) return gitRun({QStringLiteral("stash"), QStringLiteral("drop")});
    return gitRun({QStringLiteral("stash")});
}

ToolResult toolGitPull(const QJsonObject& a) {
    QStringList args{QStringLiteral("pull")};
    if (argBool(a, {"rebase"})) args << QStringLiteral("--rebase");
    return gitRun(args);
}

ToolResult toolGitPush(const QJsonObject& a) {
    // A crew coder works inside a throwaway folder-copy of the project — .git and
    // all. A push from there would send unreviewed, un-landed sandbox work straight
    // to your real remote, and it is the one git action that cannot be taken back.
    // Land the changeset through the board first, then push from the project.
    if (Tools::hasThreadRoot())
        return fail(QStringLiteral(
            "refusing to push from a crew sandbox — let the run land the changeset, then push "
            "from the project itself"));
    QStringList args{QStringLiteral("push")};
    // --force-with-lease, never a bare --force: it still lets the model rewrite a
    // branch it owns, but it refuses when the remote moved under it, which is the
    // exact case where a plain --force destroys someone else's commits.
    if (argBool(a, {"force"})) args << QStringLiteral("--force-with-lease");
    if (argBool(a, {"upstream", "set_upstream", "u"})) args << QStringLiteral("-u");
    return gitRun(args);
}

ToolResult toolGitClone(const QJsonObject& a) {
    const QString url = argStr(a, {"url", "repo", "remote"});
    if (url.isEmpty()) return fail(QStringLiteral("missing url"));
    QStringList args{QStringLiteral("clone"), url};
    const QString dir = argStr(a, {"dir", "path", "dest"});
    if (!dir.isEmpty()) {
        bool ok = false;
        const QString target = Tools::resolvePath(dir, &ok);
        if (!ok) return fail(QStringLiteral("path escapes the sandbox: %1").arg(dir));
        args << target;
    }
    // No isRepo() gate here — cloning is how a repo comes to exist.
    const GitResult r = GitFlow::git(args);
    const QString text = r.output.trimmed();
    if (!r.ok())
        return fail(text.isEmpty() ? QStringLiteral("git clone failed (exit %1)").arg(r.exit)
                                   : text);
    return okay(text.isEmpty() ? QStringLiteral("cloned") : text);
}

ToolResult toolGitRevert(const QJsonObject& a) {
    const QString ref = argStr(a, {"ref", "commit", "sha"});
    if (ref.isEmpty()) return fail(QStringLiteral("missing ref (a commit hash or a branch)"));
    QStringList args{QStringLiteral("revert")};
    if (argBool(a, {"no_commit", "noCommit"})) args << QStringLiteral("--no-commit");
    args << ref;
    return gitRun(args);
}

ToolResult toolGitCherryPick(const QJsonObject& a) {
    const QString ref = argStr(a, {"ref", "commit", "sha"});
    if (ref.isEmpty()) return fail(QStringLiteral("missing ref (a commit hash or a branch)"));
    QStringList args{QStringLiteral("cherry-pick")};
    if (argBool(a, {"no_commit", "noCommit"})) args << QStringLiteral("--no-commit");
    args << ref;
    return gitRun(args);
}

// ======================================================== background shells
//
// A long job — a build, a test suite, a dev server — must not wedge the agent for
// its whole duration. `bg` starts it and returns at once; the model reads it with
// `bash_output`, blocks on it with `wait_bg`, and stops it with `kill_bash`.
//
// State is three files per job, so it survives the agent restarting:
//   <id>.log   merged stdout+stderr
//   <id>.pid   the child's pid
//   <id>.off   how many bytes of the log the model has already been shown

QString bgDir() {
    const QString d = QDir::tempPath() + QStringLiteral("/ollamadev-bg");
    QDir().mkpath(d);
    return d;
}

bool bgAlive(qint64 pid) {
    if (pid <= 0) return false;
#ifdef Q_OS_WIN
    return false;
#else
    // Signal 0: asks the kernel "could I signal this pid?" without sending anything.
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

QString bgRead(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

void bgWrite(const QString& path, const QString& text) {
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(text.toUtf8());
}

// The id is derived from the caller's own arguments, never from anything a job id
// could smuggle in: basename() strips any path so `bash_output("../../etc/passwd")`
// reads a file in the bg dir or nothing at all.
QString bgJobId(const QJsonObject& a) {
    return QFileInfo(argStr(a, {"bg_id", "id", "job"})).fileName();
}

ToolResult toolBg(const QJsonObject& a) {
    QString cmd = argStr(a, {"command", "cmd"}).trimmed();
    while (cmd.endsWith(QLatin1Char('&'))) cmd.chop(1);  // it is already backgrounded
    cmd = cmd.trimmed();
    if (cmd.isEmpty()) return fail(QStringLiteral("missing command"));

    const QString id =
        QStringLiteral("bg_") +
        QString::fromLatin1(
            QCryptographicHash::hash((cmd + QString::number(QDateTime::currentMSecsSinceEpoch()))
                                         .toUtf8(),
                                     QCryptographicHash::Sha1)
                .toHex()
                .left(6));
    const QString log = bgDir() + QStringLiteral("/") + id + QStringLiteral(".log");
    // Create the log BEFORE the job starts. startDetached() returns as soon as the
    // shell is forked, which is before that shell has opened the redirect — so a
    // bash_output or wait_bg called immediately (the normal thing to do!) would
    // look for a log that does not exist yet and report "no such background job"
    // for a job that is running perfectly well.
    bgWrite(log, QString());

    QProcess p;
#ifdef Q_OS_WIN
    p.setProgram(QStringLiteral("cmd"));
    p.setArguments({QStringLiteral("/C"), cmd + QStringLiteral(" > \"") + log +
                                              QStringLiteral("\" 2>&1")});
#else
    // The log path is ours (temp dir + a hex id), but single-quote it anyway: a
    // temp dir with a space in it would otherwise silently split the redirect.
    p.setProgram(QStringLiteral("/bin/sh"));
    p.setArguments({QStringLiteral("-c"), QStringLiteral("(%1) > '%2' 2>&1").arg(cmd, log)});
#endif
    // The job inherits the CALLER's root: a crew coder's background build must run
    // in that coder's sandbox, not in the user's project.
    p.setWorkingDirectory(Tools::threadRoot());

    qint64 pid = 0;
    if (!p.startDetached(&pid))
        return fail(QStringLiteral("could not start the background job"));
    // We have the pid synchronously, so — unlike the PHP original, which slept 60ms
    // and hoped the shell had written it — there is no window where the job is
    // running but unknown, and no way for wait_bg to call it "finished" at 0s.
    bgWrite(bgDir() + QStringLiteral("/") + id + QStringLiteral(".pid"), QString::number(pid));

    return okay(QStringLiteral("started background job %1 (pid %2)\n"
                               "  read it:  bash_output(bg_id=\"%1\")\n"
                               "  wait:     wait_bg(bg_id=\"%1\")\n"
                               "  stop it:  kill_bash(bg_id=\"%1\")\n"
                               "  command:  %3")
                    .arg(id)
                    .arg(pid)
                    .arg(cmd));
}

ToolResult toolBashOutput(const QJsonObject& a) {
    const QString id = bgJobId(a);
    const QString base = bgDir() + QStringLiteral("/") + id;
    if (id.isEmpty() || !QFileInfo(base + QStringLiteral(".log")).isFile())
        return fail(QStringLiteral("no such background job: %1").arg(id.isEmpty() ? QStringLiteral("(none)") : id));

    const QString content = bgRead(base + QStringLiteral(".log"));
    // A cursor on disk, so each call shows only what is NEW. If the log shrank
    // (something truncated or rotated it) the cursor is meaningless — start over
    // rather than slicing past the end.
    int off = bgRead(base + QStringLiteral(".off")).toInt();
    if (off < 0 || off > content.size()) off = 0;
    const QString fresh = content.mid(off);
    bgWrite(base + QStringLiteral(".off"), QString::number(content.size()));

    const qint64 pid = bgRead(base + QStringLiteral(".pid")).trimmed().toLongLong();
    const QString state = bgAlive(pid) ? QStringLiteral("running") : QStringLiteral("finished");
    return okay(QStringLiteral("%1\n[%2 — %3]")
                    .arg(fresh.isEmpty() ? QStringLiteral("(no new output)") : fresh, id, state));
}

ToolResult toolKillBash(const QJsonObject& a) {
    const QString id = bgJobId(a);
    const QString base = bgDir() + QStringLiteral("/") + id;
    if (id.isEmpty() || !QFileInfo(base + QStringLiteral(".log")).isFile())
        return fail(QStringLiteral("no such background job: %1").arg(id.isEmpty() ? QStringLiteral("(none)") : id));

    const qint64 pid = bgRead(base + QStringLiteral(".pid")).trimmed().toLongLong();
    if (pid <= 0) return fail(QStringLiteral("no pid recorded for %1").arg(id));
    if (!bgAlive(pid)) return okay(QStringLiteral("background job %1 had already finished").arg(id));
#ifndef Q_OS_WIN
    // OUR OWN CHILD, by the pid we recorded when we started it. Never a kill by
    // name — no pkill, no killall: this process does not get to decide that every
    // `node` on the machine belongs to it.
    ::kill(static_cast<pid_t>(pid), SIGTERM);
#endif
    return okay(QStringLiteral("stopped background job %1 (pid %2)").arg(id).arg(pid));
}

ToolResult toolWaitBg(const QJsonObject& a) {
    const int secs = qBound(1, argInt(a, {"seconds", "timeout", "max"}, 60), 600);
    const QString id = bgJobId(a);
    if (id.isEmpty()) return fail(QStringLiteral("missing bg_id"));

    const QString base = bgDir() + QStringLiteral("/") + id;
    if (!QFileInfo(base + QStringLiteral(".log")).isFile())
        return fail(QStringLiteral("no such background job: %1").arg(id));
    const qint64 pid = bgRead(base + QStringLiteral(".pid")).trimmed().toLongLong();
    if (pid <= 0) return fail(QStringLiteral("no pid recorded for %1").arg(id));

    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < qint64(secs) * 1000) {
        if (!bgAlive(pid))
            return okay(QStringLiteral("background job %1 finished after %2s — read it with "
                                       "bash_output(bg_id=\"%1\")")
                            .arg(id)
                            .arg(clock.elapsed() / 1000));
        QThread::msleep(200);
    }
    return okay(QStringLiteral("background job %1 is still running after %2s").arg(id).arg(secs));
}

// ===================================================================== calc
//
// A real recursive-descent parser over + - * / % ^ ( ) and unary minus. The PHP
// original filtered the string and handed it to eval(), which fataled the whole
// process on `1/0` and on any malformed input — a model typo could kill the run.
// Nothing here can do worse than return "invalid expression".

struct Calc {
    QString s;
    int i = 0;
    bool bad = false;
    bool divZero = false;

    void skip() {
        while (i < s.size() && s.at(i).isSpace()) ++i;
    }
    bool eat(QChar c) {
        skip();
        if (i < s.size() && s.at(i) == c) {
            ++i;
            return true;
        }
        return false;
    }
    double expr() {  // + -
        double v = term();
        for (;;) {
            if (eat(QLatin1Char('+')))
                v += term();
            else if (eat(QLatin1Char('-')))
                v -= term();
            else
                return v;
        }
    }
    double term() {  // * / %
        double v = power();
        for (;;) {
            if (eat(QLatin1Char('*'))) {
                v *= power();
            } else if (eat(QLatin1Char('/'))) {
                const double d = power();
                if (d == 0.0) {
                    divZero = true;
                    return 0.0;
                }
                v /= d;
            } else if (eat(QLatin1Char('%'))) {
                const double d = power();
                if (d == 0.0) {
                    divZero = true;
                    return 0.0;
                }
                v = std::fmod(v, d);
            } else {
                return v;
            }
        }
    }
    double power() {  // ^, right-associative
        const double b = unary();
        if (eat(QLatin1Char('^'))) return std::pow(b, power());
        return b;
    }
    double unary() {
        if (eat(QLatin1Char('-'))) return -unary();
        if (eat(QLatin1Char('+'))) return unary();
        return atom();
    }
    double atom() {
        skip();
        if (eat(QLatin1Char('('))) {
            const double v = expr();
            if (!eat(QLatin1Char(')'))) bad = true;
            return v;
        }
        const int start = i;
        while (i < s.size() && (s.at(i).isDigit() || s.at(i) == QLatin1Char('.'))) ++i;
        if (i == start) {
            bad = true;
            return 0.0;
        }
        bool ok = false;
        const double v = s.mid(start, i - start).toDouble(&ok);
        if (!ok) bad = true;
        return v;
    }
};

ToolResult toolCalc(const QJsonObject& a) {
    const QString expr =
        argStr(a, {"expr", "expression", "calculation", "equation", "value"}).trimmed();
    if (expr.isEmpty()) return fail(QStringLiteral("missing expression"));

    Calc c;
    c.s = expr;
    const double v = c.expr();
    c.skip();
    if (c.divZero) return fail(QStringLiteral("division by zero"));
    if (c.bad || c.i != c.s.size())
        return fail(QStringLiteral("invalid expression: %1").arg(expr));
    if (!std::isfinite(v)) return fail(QStringLiteral("result is not a finite number"));

    // An integer result should read as one: "4", not "4.0000000000".
    if (qFuzzyCompare(v, std::floor(v)) && std::fabs(v) < 1e15)
        return okay(QString::number(static_cast<qint64>(v)));
    return okay(QString::number(v, 'g', 12));
}

// ================================================================= ask_user

ToolResult toolAskUser(const QJsonObject& a) {
    const QString q = argStr(a, {"question", "prompt", "text"}).trimmed();
    if (q.isEmpty()) return fail(QStringLiteral("missing question"));

    QStringList options;
    const QJsonValue ov = a.value(QLatin1String("options"));
    if (ov.isArray()) {
        for (const auto& o : ov.toArray())
            if (!o.toString().trimmed().isEmpty()) options << o.toString().trimmed();
    } else {
        const QString s = argStr(a, {"options", "choices"});
        for (const QString& o : s.split(QLatin1Char(','), Qt::SkipEmptyParts))
            if (!o.trimmed().isEmpty()) options << o.trimmed();
    }

    bool asked = false;
    const QString answer = Tools::askQuestion(q, options, &asked);
    if (!asked)
        return okay(QStringLiteral(
            "Nobody can answer right now (this run is not interactive). Proceed with the most "
            "reasonable default and state the assumption you made."));
    return okay(answer.isEmpty() ? QStringLiteral("the user did not answer")
                                 : QStringLiteral("the user answered: %1").arg(answer));
}

}  // namespace

void Tools::registerAll() {
    Registry& r = registry();
    QMutexLocker lock(&r.mutex);
    if (r.populated) return;
    r.populated = true;

    // TODO: not yet ported from the PHP registry (src/65-tools-register.php):
    //   clear_board        — needs Board/Crew, and must stay refused mid-run

    add(r, ToolDef{QStringLiteral("view"),
                   QStringLiteral("Read a file with line numbers."),
                   params(QJsonObject{{"file_path", strProp(QStringLiteral("Path to the file to read"))},
                                      {"offset", intProp(QStringLiteral("Start line (0-based, optional)"))},
                                      {"limit", intProp(QStringLiteral("Maximum number of lines (optional)"))}},
                          {QStringLiteral("file_path")}),
                   false, toolView});

    add(r, ToolDef{QStringLiteral("write"),
                   QStringLiteral("Create a new file or overwrite an existing file with the given content."),
                   params(QJsonObject{{"file_path", strProp(QStringLiteral("Path of the file to write"))},
                                      {"content", strProp(QStringLiteral("Full content to write to the file"))}},
                          {QStringLiteral("file_path"), QStringLiteral("content")}),
                   true, toolWrite});

    add(r, ToolDef{QStringLiteral("edit"),
                   QStringLiteral("Replace the first occurrence of old_string with new_string in a file."),
                   params(QJsonObject{{"file_path", strProp(QStringLiteral("Path of the file to edit"))},
                                      {"old_string", strProp(QStringLiteral("Exact existing text to replace"))},
                                      {"new_string", strProp(QStringLiteral("Replacement text"))}},
                          {QStringLiteral("file_path"), QStringLiteral("old_string"),
                           QStringLiteral("new_string")}),
                   true, toolEdit});

    add(r, ToolDef{QStringLiteral("multi_edit"),
                   QStringLiteral("Apply several edits to ONE file in a single atomic operation (all "
                                  "apply or none). Prefer this over multiple edit calls on the same file."),
                   params(QJsonObject{{"file_path", strProp(QStringLiteral("Path of the file to edit"))},
                                      {"edits", arrProp(QStringLiteral(
                                                    "Edits applied in order: "
                                                    "[{\"old_string\":..,\"new_string\":..,\"replace_all\":false}]"))}},
                          {QStringLiteral("file_path"), QStringLiteral("edits")}),
                   true, toolMultiEdit});

    add(r, ToolDef{QStringLiteral("mkdir"),
                   QStringLiteral("Create a directory. Use parents=true when parent directories may not exist."),
                   params(QJsonObject{{"path", strProp(QStringLiteral("Directory path to create"))},
                                      {"parents", boolProp(QStringLiteral("Create parent directories as needed"))}},
                          {QStringLiteral("path")}),
                   true, toolMkdir});

    add(r, ToolDef{QStringLiteral("touch"),
                   QStringLiteral("Create an empty file or update its modification timestamp."),
                   params(QJsonObject{{"path", strProp(QStringLiteral("File path to create or update"))}},
                          {QStringLiteral("path")}),
                   true, toolTouch});

    add(r, ToolDef{QStringLiteral("cp"), QStringLiteral("Copy a file or directory."),
                   params(QJsonObject{{"src", strProp(QStringLiteral("Source path"))},
                                      {"dst", strProp(QStringLiteral("Destination path"))}},
                          {QStringLiteral("src"), QStringLiteral("dst")}),
                   true, toolCp});

    add(r, ToolDef{QStringLiteral("mv"), QStringLiteral("Move or rename a file or directory."),
                   params(QJsonObject{{"src", strProp(QStringLiteral("Source path"))},
                                      {"dst", strProp(QStringLiteral("Destination path"))}},
                          {QStringLiteral("src"), QStringLiteral("dst")}),
                   true, toolMv});

    add(r, ToolDef{QStringLiteral("rm"),
                   QStringLiteral("Remove a file or directory. Use recursive=true for directories."),
                   params(QJsonObject{{"path", strProp(QStringLiteral("Path to remove"))},
                                      {"recursive", boolProp(QStringLiteral("Remove directories recursively"))},
                                      {"dry_run", boolProp(QStringLiteral(
                                                      "Preview what would be removed without deleting it"))}},
                          {QStringLiteral("path")}),
                   true, toolRm});

    add(r, ToolDef{QStringLiteral("patch"), QStringLiteral("Apply a unified diff patch to a file."),
                   params(QJsonObject{{"file_path", strProp(QStringLiteral("Path of the file to patch"))},
                                      {"diff", strProp(QStringLiteral("Unified diff to apply"))}},
                          {QStringLiteral("file_path"), QStringLiteral("diff")}),
                   true, toolPatch});

    add(r, ToolDef{QStringLiteral("ls"), QStringLiteral("List the contents of a directory."),
                   params(QJsonObject{{"path", strProp(QStringLiteral(
                                                   "Directory path (defaults to current directory)"))}},
                          {}),
                   false, toolLs});

    add(r, ToolDef{QStringLiteral("grep"),
                   QStringLiteral("Search file contents recursively for a regular-expression pattern."),
                   params(QJsonObject{{"pattern", strProp(QStringLiteral("Regex pattern to search for"))},
                                      {"path", strProp(QStringLiteral(
                                                   "Directory or file to search (defaults to .)"))},
                                      {"include", strProp(QStringLiteral("Optional glob filter, e.g. *.cpp"))}},
                          {QStringLiteral("pattern")}),
                   false, toolGrep});

    add(r, ToolDef{QStringLiteral("glob"), QStringLiteral("Find files matching a glob pattern."),
                   params(QJsonObject{{"pattern", strProp(QStringLiteral("Glob pattern, e.g. **/*.cpp"))},
                                      {"path", strProp(QStringLiteral("Base directory (defaults to .)"))}},
                          {QStringLiteral("pattern")}),
                   false, toolGlob});

    add(r, ToolDef{QStringLiteral("bash"), QStringLiteral("Run a shell command and return its output."),
                   params(QJsonObject{{"command", strProp(QStringLiteral("The shell command to execute"))}},
                          {QStringLiteral("command")}),
                   true, toolBash});

    add(r, ToolDef{QStringLiteral("todo_write"),
                   QStringLiteral("Create or replace the session todo list to plan and track multi-step "
                                  "work. Pass the FULL list each time; mark items "
                                  "in_progress/completed as you go."),
                   params(QJsonObject{{"todos", arrProp(QStringLiteral(
                                                    "Full list: [{\"content\":\"..\",\"status\":"
                                                    "\"pending|in_progress|completed\",\"activeForm\":\"..\"}]"))}},
                          {QStringLiteral("todos")}),
                   false, toolTodoWrite});

    // ---- git ----------------------------------------------------------------
    // The read-only six are mutates=false, so they never prompt and stay usable in
    // plan/readonly mode — reading history is how a model decides what to do.
    // Everything below them moves HEAD, the index, or a remote.
    add(r, ToolDef{QStringLiteral("git_status"),
                   QStringLiteral("Show the working tree status (short format)."),
                   params(QJsonObject{}, {}), false, toolGitStatus});

    add(r, ToolDef{QStringLiteral("git_diff"),
                   QStringLiteral("Show unstaged changes, or staged ones with cached=true."),
                   params(QJsonObject{{"file", strProp(QStringLiteral("Limit the diff to one path"))},
                                      {"cached", boolProp(QStringLiteral(
                                                     "Diff the STAGED changes instead"))}},
                          {}),
                   false, toolGitDiff});

    add(r, ToolDef{QStringLiteral("git_log"),
                   QStringLiteral("Show recent commits, one line each."),
                   params(QJsonObject{{"n", intProp(QStringLiteral("How many commits (default 10)"))}},
                          {}),
                   false, toolGitLog});

    add(r, ToolDef{QStringLiteral("git_branch"), QStringLiteral("List branches."),
                   params(QJsonObject{{"all", boolProp(QStringLiteral("Include remote branches"))}},
                          {}),
                   false, toolGitBranch});

    add(r, ToolDef{QStringLiteral("git_show"),
                   QStringLiteral("Show a commit: its message and its diff."),
                   params(QJsonObject{{"ref", strProp(QStringLiteral(
                                                  "Commit hash, tag or branch (default HEAD)"))},
                                      {"stat", boolProp(QStringLiteral(
                                                   "Only the file summary, not the full diff"))}},
                          {}),
                   false, toolGitShow});

    add(r, ToolDef{QStringLiteral("git_remote"), QStringLiteral("List the configured remotes."),
                   params(QJsonObject{}, {}), false, toolGitRemote});

    add(r, ToolDef{QStringLiteral("git_fetch"),
                   QStringLiteral("Fetch from a remote without merging anything."),
                   params(QJsonObject{{"all", boolProp(QStringLiteral("Fetch every remote"))},
                                      {"prune", boolProp(QStringLiteral(
                                                    "Drop refs whose remote branch is gone"))}},
                          {}),
                   true, toolGitFetch});

    add(r, ToolDef{QStringLiteral("git_add"),
                   QStringLiteral("Stage changes for the next commit."),
                   params(QJsonObject{{"files", strProp(QStringLiteral(
                                                    "Path(s) to stage (default: everything here)"))},
                                      {"all", boolProp(QStringLiteral(
                                                  "Stage every change in the repo"))}},
                          {}),
                   true, toolGitAdd});

    add(r, ToolDef{QStringLiteral("git_commit"),
                   QStringLiteral("Commit the staged changes. Write a real message: what changed "
                                  "and why, not just the file names."),
                   params(QJsonObject{{"message", strProp(QStringLiteral("The commit message"))},
                                      {"all", boolProp(QStringLiteral(
                                                  "Stage every tracked change first (-a)"))},
                                      {"amend", boolProp(QStringLiteral(
                                                    "Replace the previous commit instead"))}},
                          {QStringLiteral("message")}),
                   true, toolGitCommit});

    add(r, ToolDef{QStringLiteral("git_checkout"),
                   QStringLiteral("Switch to a branch, or create one with new=true."),
                   params(QJsonObject{{"branch", strProp(QStringLiteral("Branch name"))},
                                      {"new", boolProp(QStringLiteral("Create the branch (-b)"))}},
                          {QStringLiteral("branch")}),
                   true, toolGitCheckout});

    add(r, ToolDef{QStringLiteral("git_merge"), QStringLiteral("Merge a branch into the current one."),
                   params(QJsonObject{{"branch", strProp(QStringLiteral("Branch to merge in"))},
                                      {"no_ff", boolProp(QStringLiteral("Always make a merge commit"))},
                                      {"squash", boolProp(QStringLiteral(
                                                     "Collapse it into one staged change"))}},
                          {QStringLiteral("branch")}),
                   true, toolGitMerge});

    add(r, ToolDef{QStringLiteral("git_rebase"), QStringLiteral("Rebase the current branch."),
                   params(QJsonObject{{"branch", strProp(QStringLiteral("Rebase onto this branch"))},
                                      {"onto", strProp(QStringLiteral("Explicit --onto target"))}},
                          {QStringLiteral("branch")}),
                   true, toolGitRebase});

    add(r, ToolDef{QStringLiteral("git_stash"),
                   QStringLiteral("Shelve the working changes; pop/list/drop them."),
                   params(QJsonObject{{"pop", boolProp(QStringLiteral("Restore the newest stash"))},
                                      {"list", boolProp(QStringLiteral("List the stashes"))},
                                      {"drop", boolProp(QStringLiteral("Delete the newest stash"))}},
                          {}),
                   true, toolGitStash});

    add(r, ToolDef{QStringLiteral("git_revert"),
                   QStringLiteral("Undo a commit by making a new one that reverses it."),
                   params(QJsonObject{{"ref", strProp(QStringLiteral("Commit to revert"))},
                                      {"no_commit", boolProp(QStringLiteral(
                                                        "Stage the reversal without committing"))}},
                          {QStringLiteral("ref")}),
                   true, toolGitRevert});

    add(r, ToolDef{QStringLiteral("git_cherry_pick"),
                   QStringLiteral("Apply one commit from elsewhere onto this branch."),
                   params(QJsonObject{{"ref", strProp(QStringLiteral("Commit to apply"))},
                                      {"no_commit", boolProp(QStringLiteral(
                                                        "Stage it without committing"))}},
                          {QStringLiteral("ref")}),
                   true, toolGitCherryPick});

    add(r, ToolDef{QStringLiteral("git_pull"), QStringLiteral("Fetch and integrate the remote branch."),
                   params(QJsonObject{{"rebase", boolProp(QStringLiteral("Rebase instead of merge"))}},
                          {}),
                   true, toolGitPull});

    add(r, ToolDef{QStringLiteral("git_push"),
                   QStringLiteral("Push commits to the remote. This LEAVES THE MACHINE and is the "
                                  "one git action that cannot be undone — be sure first."),
                   params(QJsonObject{{"upstream", boolProp(QStringLiteral(
                                                       "Set the upstream for a new branch (-u)"))},
                                      {"force", boolProp(QStringLiteral(
                                                    "Force-push (refused if the remote moved)"))}},
                          {}),
                   true, toolGitPush});

    add(r, ToolDef{QStringLiteral("git_clone"), QStringLiteral("Clone a repository."),
                   params(QJsonObject{{"url", strProp(QStringLiteral("Repository URL"))},
                                      {"dir", strProp(QStringLiteral("Directory to clone into"))}},
                          {QStringLiteral("url")}),
                   true, toolGitClone});

    // ---- background shells ---------------------------------------------------
    add(r, ToolDef{QStringLiteral("bg"),
                   QStringLiteral("Start a long command in the background and return immediately. "
                                  "Use this for builds, test suites and servers instead of bash, "
                                  "which blocks until the command exits."),
                   params(QJsonObject{{"command", strProp(QStringLiteral("The shell command to run"))}},
                          {QStringLiteral("command")}),
                   true, toolBg});

    add(r, ToolDef{QStringLiteral("bash_output"),
                   QStringLiteral("Read whatever a background job has printed SINCE THE LAST READ, "
                                  "and whether it is still running."),
                   params(QJsonObject{{"bg_id", strProp(QStringLiteral("The job id bg returned"))}},
                          {QStringLiteral("bg_id")}),
                   false, toolBashOutput});

    add(r, ToolDef{QStringLiteral("wait_bg"),
                   QStringLiteral("Block until a background job finishes, or until the timeout."),
                   params(QJsonObject{{"bg_id", strProp(QStringLiteral("The job id bg returned"))},
                                      {"seconds", intProp(QStringLiteral(
                                                      "How long to wait (default 60, max 600)"))}},
                          {QStringLiteral("bg_id")}),
                   false, toolWaitBg});

    add(r, ToolDef{QStringLiteral("kill_bash"), QStringLiteral("Stop a background job."),
                   params(QJsonObject{{"bg_id", strProp(QStringLiteral("The job id bg returned"))}},
                          {QStringLiteral("bg_id")}),
                   true, toolKillBash});

    // ---- calc ----------------------------------------------------------------
    add(r, ToolDef{QStringLiteral("calc"),
                   QStringLiteral("Evaluate an arithmetic expression exactly. Use it instead of "
                                  "doing arithmetic in your head — you are bad at it."),
                   params(QJsonObject{{"expr", strProp(QStringLiteral(
                                                   "e.g. (1024 * 3) / 7 or 2^16"))}},
                          {QStringLiteral("expr")}),
                   false, toolCalc});

    // ---- ask_user ------------------------------------------------------------
    // mutates=false: asking a question changes nothing on disk, and a model in plan
    // mode is exactly when a question is most useful. It cannot hang a headless run
    // — with no asker installed it returns immediately (see toolAskUser).
    add(r, ToolDef{QStringLiteral("ask_user"),
                   QStringLiteral("Ask the human a question when the task is genuinely ambiguous "
                                  "and the answer changes what you would do. Do NOT ask for things "
                                  "you can find out yourself by reading the code."),
                   params(QJsonObject{{"question", strProp(QStringLiteral("The question to ask"))},
                                      {"options", arrProp(QStringLiteral(
                                                      "Optional choices to offer, as strings"))}},
                          {QStringLiteral("question")}),
                   false, toolAskUser});

    // mutates=false on purpose: this is the ONE tool that must stay callable in
    // plan mode. It changes no files — it asks the user to approve the plan, and
    // only their yes lifts the mode.
    add(r, ToolDef{QStringLiteral("exit_plan_mode"),
                   QStringLiteral("Call this ONLY in plan mode, after you have researched (read-only) "
                                  "and are ready to act. Presents your plan to the user for approval; "
                                  "on yes, plan mode ends and you may edit. Do NOT call it for pure "
                                  "research/answer tasks."),
                   params(QJsonObject{{"plan", strProp(QStringLiteral(
                                                   "The plan: the concrete steps you intend to take, "
                                                   "in markdown."))}},
                          {QStringLiteral("plan")}),
                   false, toolExitPlanMode});

    // Skills + graph memory. The system prompt lists only skill names and memory
    // slugs; these three tools are how the model pulls the actual content, and
    // are the whole reason a 9b local model can work with a large skill library
    // without drowning in it.
    add(r, ToolDef{QStringLiteral("skill"),
                   QStringLiteral("Load a skill's full instructions on demand. Call this as soon "
                                  "as a listed skill applies to the task, BEFORE you start."),
                   params(QJsonObject{{"name", strProp(QStringLiteral(
                                                   "Name of the skill to load, as listed in the "
                                                   "skills catalog"))}},
                          {QStringLiteral("name")}),
                   false, toolSkill});

    add(r, ToolDef{QStringLiteral("recall"),
                   QStringLiteral("Read project memory: with no argument, list what is remembered; "
                                  "with slug, read one note in full; with query, search notes."),
                   params(QJsonObject{{"slug", strProp(QStringLiteral("Read this note in full"))},
                                      {"query", strProp(QStringLiteral(
                                                    "Search notes by title, tag, or content"))}},
                          {}),
                   false, toolRecall});

    add(r, ToolDef{QStringLiteral("remember"),
                   QStringLiteral("Persist a durable, reusable fact about this project (an "
                                  "architecture decision, a convention, a gotcha). Link related "
                                  "notes with [[slug]]. Not for transient task details."),
                   params(QJsonObject{{"title", strProp(QStringLiteral("Short title for the note"))},
                                      {"content", strProp(QStringLiteral(
                                                      "The fact, in 1-3 sentences. Use [[slug]] to "
                                                      "link related notes."))},
                                      {"tags", strProp(QStringLiteral(
                                                   "Optional comma-separated tags"))}},
                          {QStringLiteral("content")}),
                   true, toolRemember});

    // ---- network ----------------------------------------------------------
    // mutates=true is deliberate for both. Nothing on disk changes, but the query
    // (or the URL) LEAVES THE MACHINE, and that is a side effect a read-only or
    // plan-mode session never agreed to. It is also what stops an MCP client —
    // read-only by default — from using us as an open web proxy.

    add(r, ToolDef{QStringLiteral("search"),
                   QStringLiteral("Search the web and return result titles, URLs, and snippets."),
                   params(QJsonObject{{"query", strProp(QStringLiteral("What to search for"))},
                                      {"limit", intProp(QStringLiteral("Max results, 1-10 (default 5)"))},
                                      {"provider", strProp(QStringLiteral(
                                                       "duckduckgo | searxng | brave (default: "
                                                       "config search.provider)"))}},
                          {QStringLiteral("query")}),
                   true, toolSearch});

    add(r, ToolDef{QStringLiteral("fetch"),
                   QStringLiteral("Fetch a URL over HTTP(S) and return its readable text."),
                   params(QJsonObject{{"url", strProp(QStringLiteral("The URL to fetch"))},
                                      {"timeout", intProp(QStringLiteral("Seconds (default 30)"))}},
                          {QStringLiteral("url")}),
                   true, toolFetch});

    // ---- local semantic index ---------------------------------------------
    add(r, ToolDef{QStringLiteral("code_search"),
                   QStringLiteral("Search THIS repository by meaning rather than by literal text — "
                                  "use it when you do not know the exact identifier to grep for. "
                                  "Requires `ollamadev index build` first."),
                   params(QJsonObject{{"query", strProp(QStringLiteral(
                                                    "What the code should DO, in plain words"))},
                                      {"limit", intProp(QStringLiteral("Max hits, 1-20 (default 8)"))}},
                          {QStringLiteral("query")}),
                   false, toolCodeSearch});

    // Running a suite executes arbitrary project code (and its build), so it is a
    // mutation, not a read.
    add(r, ToolDef{QStringLiteral("run_tests"),
                   QStringLiteral("Detect and run this project's test suite; returns pass/fail and "
                                  "the tail of the output."),
                   params(QJsonObject{}, {}),  // no parameters → MUST be {}: see the QUIRK in Tools.h
                   true, toolRunTests});

    // ---- delegation --------------------------------------------------------
    // task/subagent/delegate: a nested agent for focused sub-tasks. Provided by
    // SubAgent so its runner and its schema stay in one place, added here because
    // the registry's add() is file-local to this translation unit.
    for (const ToolDef& d : SubAgent::tools()) add(r, d);

    // ---- MCP ---------------------------------------------------------------
    // Tools discovered on the servers in config `mcpServers`, registered as
    // first-class tools so the model calls a remote one exactly like a local one.
    // Returns immediately, spawning nothing, when no servers are configured.
    for (const ToolDef& d : Mcp::discoverTools()) add(r, d);
}

const ToolDef* Tools::find(const QString& name) {
    registerAll();
    Registry& r = registry();
    QMutexLocker lock(&r.mutex);
    const auto it = r.index.constFind(name);
    if (it == r.index.constEnd()) return nullptr;
    // Safe to hand out: `defs` is only ever appended to inside registerAll(),
    // which runs exactly once, so the elements never move after population.
    return &r.defs[it.value()];
}

QJsonArray Tools::schemas() {
    registerAll();
    Registry& r = registry();
    QMutexLocker lock(&r.mutex);
    QJsonArray out;
    for (const ToolDef& d : r.defs) {
        out.append(QJsonObject{
            {"type", "function"},
            {"function", QJsonObject{{"name", d.name},
                                     {"description", d.description},
                                     {"parameters", d.parameters}}}});
    }
    return out;
}

QStringList Tools::names() {
    registerAll();
    Registry& r = registry();
    QMutexLocker lock(&r.mutex);
    QStringList out;
    out.reserve(r.defs.size());
    for (const ToolDef& d : r.defs) out << d.name;
    return out;
}

ToolResult Tools::run(const QString& name, const QJsonObject& args) {
    const ToolDef* t = find(name);
    if (!t)
        return fail(QStringLiteral("'%1' is not a valid tool. Available: %2")
                        .arg(name, names().join(QStringLiteral(", "))));
    if (!Permission::check(*t, args))
        return fail(QStringLiteral("permission denied for tool '%1' (mode: %2)")
                        .arg(name, Permission::modeName(Permission::mode())));
    if (!t->fn) return fail(QStringLiteral("tool '%1' has no implementation").arg(name));

    // PreToolUse hooks are a GATE: a non-zero exit blocks the tool and its output
    // becomes the reason the model is told. It fails CLOSED — a hook that cannot be
    // spawned blocks too (see Hooks.h). No hooks configured => no work done here.
    QString blocked;
    if (Hooks::preToolUse(name, args, &blocked))
        return fail(QStringLiteral("blocked by a PreToolUse hook: %1").arg(blocked));

    const ToolResult r = t->fn(args);
    Hooks::postToolUse(name, args, r.ok ? r.output : r.error);
    return r;
}

}  // namespace odv
