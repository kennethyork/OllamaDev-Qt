#include "CliBackend.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <algorithm>
#include <utility>

#include "Tools.h"  // Tools::threadRoot() — the sandbox this coder is confined to

#include "Config.h"
#include "Json.h"

namespace odv {

namespace {

// The binary name for each id. `cursor-agent` is the only one whose id is not
// literally the command, but it is — kept explicit so the mapping is one place.
QString commandFor(const QString& id) {
    if (id == "cursor-agent") return QStringLiteral("cursor-agent");
    return id;
}

// A CLI may print a banner, spinner frames and ANSI colour even in headless
// mode. The colour codes would otherwise land in the transcript verbatim.
const QRegularExpression& ansiRe() {
    static const QRegularExpression re(QStringLiteral("\x1B\\[[0-9;?]*[A-Za-z]"));
    return re;
}

}  // namespace

CliBackend::CliBackend(QString backendId)
    : id_(std::move(backendId)), command_(commandFor(id_)) {}

QStringList CliBackend::ids() {
    return {QStringLiteral("claude"),   QStringLiteral("codex"),  QStringLiteral("gemini"),
            QStringLiteral("cursor-agent"), QStringLiteral("opencode"), QStringLiteral("qwen"),
            QStringLiteral("aider"),    QStringLiteral("goose"),  QStringLiteral("amp"),
            QStringLiteral("crush"),    QStringLiteral("droid")};
}

QString CliBackend::labelFor(const QString& backendId) {
    if (backendId == "claude") return QStringLiteral("Claude Code");
    if (backendId == "codex") return QStringLiteral("Codex");
    if (backendId == "gemini") return QStringLiteral("Gemini CLI");
    if (backendId == "cursor-agent") return QStringLiteral("Cursor Agent");
    if (backendId == "opencode") return QStringLiteral("OpenCode");
    if (backendId == "qwen") return QStringLiteral("Qwen Code");
    if (backendId == "aider") return QStringLiteral("Aider");
    if (backendId == "goose") return QStringLiteral("Goose");
    if (backendId == "amp") return QStringLiteral("Amp");
    if (backendId == "crush") return QStringLiteral("Crush");
    if (backendId == "droid") return QStringLiteral("Droid");
    return backendId;
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

// PATH as seen by a GUI app launched from a desktop menu is NOT the PATH from
// the user's shell: ~/.local/bin and nvm's per-version bin (where half of these
// node tools live) are added by .bashrc/.zshrc, which never ran. Probing those
// explicitly is the difference between "Claude Code not installed" and it just
// working.
QStringList CliBackend::extraSearchDirs() {
    static const QStringList dirs = [] {
        const QString home = QDir::homePath();
        QStringList out{home + "/.local/bin", home + "/bin", "/usr/local/bin",
                        home + "/.bun/bin", home + "/.cargo/bin"};
        const QDir nvm(home + "/.nvm/versions/node");
        const QStringList versions =
            nvm.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
        for (const QString& v : versions) out << nvm.absoluteFilePath(v) + "/bin";
        return out;
    }();
    return dirs;
}

QString CliBackend::executable() {
    QMutexLocker lock(&probeMutex_);
    if (probed_) return exePath_;

    // A user-pinned absolute path wins (cli.<id>.path), then PATH, then the
    // directories a login shell would have added.
    const QString pinned = Config::str(QStringLiteral("cli.%1.path").arg(id_));
    if (!pinned.isEmpty() && QFileInfo(pinned).isExecutable()) {
        exePath_ = pinned;
    } else {
        exePath_ = QStandardPaths::findExecutable(command_);
        if (exePath_.isEmpty())
            exePath_ = QStandardPaths::findExecutable(command_, extraSearchDirs());
    }
    probed_ = true;
    return exePath_;
}

bool CliBackend::available() { return !executable().isEmpty(); }

// These CLIs choose their own model from their own config/auth, and none of
// them can enumerate models without a network round-trip (and, for most, an
// interactive login). ":default" is the sentinel for "let the CLI decide" and
// is stripped before argv is built; any other string is passed through to the
// CLI's verified model flag.
QStringList CliBackend::models() { return {id_ + QStringLiteral(":default")}; }

QString CliBackend::defaultModel() {
    const QString cfg = Config::str(QStringLiteral("cli.%1.model").arg(id_));
    return cfg.isEmpty() ? id_ + QStringLiteral(":default") : cfg;
}

// ---------------------------------------------------------------------------
// argv
// ---------------------------------------------------------------------------

bool CliBackend::promptOnStdin() const { return id_ == "claude" || id_ == "codex"; }

bool CliBackend::usesLastMessageFile() const { return id_ == "codex"; }

// VERIFIED against `--help` on this machine (claude, codex, gemini,
// cursor-agent, opencode, qwen). The rest are the documented invocations; they
// are not installed here, so available() gates them out rather than us guessing
// at runtime.
QStringList CliBackend::argv(const QString& model, const QString& prompt,
                             const QString& lastMessageFile) const {
    // ":default" means the CLI picks; only a real name gets a model flag.
    const QString m = (model == id_ + QStringLiteral(":default")) ? QString() : model.trimmed();
    QStringList a;

    if (id_ == "claude") {
        // claude -p --output-format text  (prompt on stdin)
        // acceptEdits, not the default mode: the crew hands it a task and
        // expects the edits to land, and a headless run has nobody to approve.
        a << QStringLiteral("-p") << QStringLiteral("--output-format") << QStringLiteral("text")
          << QStringLiteral("--permission-mode") << QStringLiteral("acceptEdits");
        if (!m.isEmpty()) a << QStringLiteral("--model") << m;
        return a;
    }
    if (id_ == "codex") {
        // codex exec ... -   (trailing '-' = read the prompt from stdin)
        // `codex exec` has no --ask-for-approval: exec never prompts, the
        // sandbox is the whole safety story, and workspace-write is what lets
        // it edit the repo it was pointed at.
        a << QStringLiteral("exec") << QStringLiteral("--skip-git-repo-check")
          << QStringLiteral("--color") << QStringLiteral("never") << QStringLiteral("--sandbox")
          << QStringLiteral("workspace-write");
        if (!m.isEmpty()) a << QStringLiteral("-m") << m;
        if (!lastMessageFile.isEmpty())
            a << QStringLiteral("--output-last-message") << lastMessageFile;
        a << QStringLiteral("-");
        return a;
    }
    if (id_ == "gemini") {
        // gemini -p <prompt> --output-format text
        // --skip-trust is required: without it gemini downgrades yolo back to
        // "default" for an untrusted folder and then blocks on an approval
        // prompt that a headless run can never answer.
        a << QStringLiteral("--output-format") << QStringLiteral("text")
          << QStringLiteral("--approval-mode") << QStringLiteral("yolo")
          << QStringLiteral("--skip-trust");
        if (!m.isEmpty()) a << QStringLiteral("-m") << m;
        a << QStringLiteral("-p") << prompt;
        return a;
    }
    if (id_ == "qwen") {
        // qwen -p <prompt> --output-format text
        // No --approval-mode on this build (the PHP port's flag is gone), and
        // auth must already be configured; --auth-type is accepted but hidden,
        // so it is only passed when the user has set it.
        a << QStringLiteral("--output-format") << QStringLiteral("text");
        const QString auth = Config::str(QStringLiteral("cli.qwen.authType"));
        if (!auth.isEmpty()) a << QStringLiteral("--auth-type") << auth;
        if (!m.isEmpty()) a << QStringLiteral("-m") << m;
        a << QStringLiteral("-p") << prompt;
        return a;
    }
    if (id_ == "cursor-agent") {
        // cursor-agent -p --output-format text --force --trust <prompt>
        a << QStringLiteral("-p") << QStringLiteral("--output-format") << QStringLiteral("text")
          << QStringLiteral("--force") << QStringLiteral("--trust");
        if (!m.isEmpty()) a << QStringLiteral("--model") << m;
        a << prompt;
        return a;
    }
    if (id_ == "opencode") {
        // opencode run --format default <prompt>
        a << QStringLiteral("run") << QStringLiteral("--format") << QStringLiteral("default")
          << QStringLiteral("--auto");
        if (!m.isEmpty()) a << QStringLiteral("-m") << m;
        a << prompt;
        return a;
    }

    // --- not installed here; documented invocations ---
    if (id_ == "aider") {
        a << QStringLiteral("--yes") << QStringLiteral("--no-pretty")
          << QStringLiteral("--no-stream");
        if (!m.isEmpty()) a << QStringLiteral("--model") << m;
        a << QStringLiteral("--message") << prompt;
        return a;
    }
    if (id_ == "goose") {
        a << QStringLiteral("run") << QStringLiteral("--text") << prompt;
        return a;
    }
    if (id_ == "amp") {
        a << QStringLiteral("-x") << prompt;
        return a;
    }
    if (id_ == "crush") {
        a << QStringLiteral("run") << QStringLiteral("-q") << prompt;
        return a;
    }
    if (id_ == "droid") {
        a << QStringLiteral("exec") << QStringLiteral("-o") << QStringLiteral("text");
        if (!m.isEmpty()) a << QStringLiteral("--model") << m;
        a << prompt;
        return a;
    }
    return a;
}

// ---------------------------------------------------------------------------
// chat
// ---------------------------------------------------------------------------

QString CliBackend::flatten(const QVector<ChatMessage>& messages) {
    // These CLIs take ONE prompt, not a role-tagged transcript, so the
    // conversation is rendered as labelled blocks and handed over whole.
    QStringList parts;
    for (const ChatMessage& m : messages) {
        const QString c = m.content.trimmed();
        if (c.isEmpty()) continue;
        parts << m.role.toUpper() + ":\n" + c;
    }
    return parts.join(QStringLiteral("\n\n"));
}

QString CliBackend::stripAnsi(const QString& s) {
    QString out = s;
    out.remove(ansiRe());
    return out;
}

ChatTurn CliBackend::chat(const QString& model, const QVector<ChatMessage>& messages,
                          const QJsonArray& toolSchemas, const StreamSink& sink,
                          const CancelToken& cancel) {
    Q_UNUSED(toolSchemas);  // see supportsNativeTools(): the CLI owns its tool loop

    ChatTurn turn;
    const QString exe = executable();
    if (exe.isEmpty()) {
        turn.error = QStringLiteral("%1 is not installed").arg(label());
        return turn;
    }

    const QString prompt = flatten(messages);
    if (prompt.isEmpty()) {
        turn.error = QStringLiteral("empty prompt");
        return turn;
    }

    // codex hands us the clean final message via a file; without it we would be
    // scraping the answer out of its event log.
    QTemporaryDir tmp;
    QString lastMsgFile;
    if (usesLastMessageFile() && tmp.isValid()) lastMsgFile = tmp.filePath("last.txt");

    // QProcess is used only through its blocking waitFor* API, which does not
    // need an event loop — so this is safe on the crew's plain worker threads,
    // and each call owns its own QProcess.
    QProcess proc;
    // SECURITY: a CLI backend is an agent in a subprocess — it does its OWN file
    // edits, so our thread-local tool root cannot confine it. Its working
    // directory IS its sandbox, and that is the only thing standing between a
    // crew coder and the user's real project.
    //
    // Getting this wrong is not a cosmetic bug: pointing it at the project root
    // would let a CLI coder write straight into the user's tree, bypassing the
    // changeset capture, the auditor, the secret gate and the overlap guard all
    // at once. Tools::threadRoot() is the sandbox the crew assigned this thread;
    // fall back to cwd only for a plain one-shot chat, where there is no sandbox
    // and the project root is genuinely what the user asked for.
    const QString root = Tools::hasThreadRoot() ? Tools::threadRoot() : QDir::currentPath();
    proc.setWorkingDirectory(root);
    proc.setProcessChannelMode(QProcess::SeparateChannels);

    // Child processes inherit our PATH, and a GUI-launched app's PATH is often
    // missing the very directories these tools (and the node runtime they need)
    // live in. Put them back before spawning.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PATH"),
               extraSearchDirs().join(QLatin1Char(':')) + QLatin1Char(':') +
                   env.value(QStringLiteral("PATH")));
    env.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
    env.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
    proc.setProcessEnvironment(env);

    proc.start(exe, argv(model, prompt, lastMsgFile));
    if (!proc.waitForStarted(15000)) {
        turn.error = QStringLiteral("failed to start %1").arg(exe);
        return turn;
    }

    if (promptOnStdin()) proc.write(prompt.toUtf8());
    // Closing stdin is mandatory either way: a CLI that reads stdin blocks
    // forever without EOF, and one that does not still checks whether stdin is
    // piped.
    proc.closeWriteChannel();

    const int timeoutMs = Config::integer("cli.timeout", 600) * 1000;
    QElapsedTimer clock;
    clock.start();

    QString out, err;
    bool cancelled = false, timedOut = false;

    while (proc.state() != QProcess::NotRunning) {
        proc.waitForReadyRead(200);

        const QByteArray o = proc.readAllStandardOutput();
        if (!o.isEmpty()) {
            const QString text = stripAnsi(QString::fromUtf8(o));
            out += text;
            if (sink.onContent && !text.isEmpty()) sink.onContent(text);
        }
        err += QString::fromUtf8(proc.readAllStandardError());

        if (cancel.cancelled()) {
            cancelled = true;
            break;
        }
        if (clock.hasExpired(timeoutMs)) {
            timedOut = true;
            break;
        }
    }

    if (cancelled || timedOut) {
        // terminate() first so the CLI can flush its session state; kill() is
        // the backstop for one that ignores SIGTERM.
        proc.terminate();
        if (!proc.waitForFinished(3000)) {
            proc.kill();
            proc.waitForFinished(2000);
        }
        turn.content = out.trimmed();
        turn.error = cancelled ? QStringLiteral("cancelled")
                               : QStringLiteral("%1 timed out after %2s")
                                     .arg(label())
                                     .arg(timeoutMs / 1000);
        return turn;
    }

    proc.waitForFinished(5000);
    {
        const QByteArray o = proc.readAllStandardOutput();
        if (!o.isEmpty()) {
            const QString text = stripAnsi(QString::fromUtf8(o));
            out += text;
            if (sink.onContent && !text.isEmpty()) sink.onContent(text);
        }
        err += QString::fromUtf8(proc.readAllStandardError());
    }

    QString content = out.trimmed();
    if (!lastMsgFile.isEmpty()) {
        QFile f(lastMsgFile);
        if (f.open(QIODevice::ReadOnly)) {
            const QString last = QString::fromUtf8(f.readAll()).trimmed();
            if (!last.isEmpty()) content = last;
        }
    }

    const int code = proc.exitCode();
    if (proc.exitStatus() != QProcess::NormalExit || (code != 0 && content.isEmpty())) {
        turn.ok = false;
        turn.error = err.trimmed().isEmpty()
                         ? QStringLiteral("%1 exited with %2").arg(label()).arg(code)
                         : stripAnsi(err).trimmed();
        return turn;
    }

    turn.ok = true;
    turn.content = content;
    // `calls` stays empty by design — the CLI already ran its own tools.
    return turn;
}

QJsonObject CliBackend::chatJson(const QString& model, const QVector<ChatMessage>& messages,
                                 const CancelToken& cancel) {
    // No format:json equivalent exists across these CLIs, so the constraint has
    // to be stated in the prompt and the reply parsed leniently — they wrap JSON
    // in ``` fences and prepend commentary just like a cloud model does.
    QVector<ChatMessage> msgs = messages;
    ChatMessage nudge;
    nudge.role = QStringLiteral("user");
    nudge.content = QStringLiteral(
        "Respond with ONLY a single valid JSON object. No prose, no code fences, "
        "no explanation before or after it. Do not edit any files.");
    msgs.append(nudge);

    StreamSink silent;
    const ChatTurn t = chat(model, msgs, {}, silent, cancel);
    if (!t.ok || t.content.isEmpty()) return {};
    return json::decodeLoose(t.content).object();
}

int CliBackend::concurrencyLimit(const QString& model) {
    Q_UNUSED(model);
    // Separate processes talking to separate remote APIs: our GPU is not in the
    // loop, so the ceiling here is the provider's rate limit, not our hardware.
    return std::max(1, Config::integer(QStringLiteral("cli.%1.parallel").arg(id_), 4));
}

}  // namespace odv
