#include "Repl.h"

#include "Hooks.h"  // UserCmds — custom slash commands

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimer>

#include <csignal>
#include <cstdio>
#include <unistd.h>

#include "AgentDefs.h"
#include "Config.h"
#include "Crew.h"
#include "Models.h"
#include "OllamaBackend.h"
#include "Tools.h"
#include "Usage.h"
#include "Version.h"
#include "Vision.h"

namespace odv {
namespace {

// SIGINT during a response must not kill the process — it cancels the TURN. The
// handler therefore does the only thing a handler may safely do (set a flag);
// a 50ms timer inside the REPL turns that flag into a CancelToken cancel, which
// the backend checks between chunks and the agent checks between tool
// iterations. Nothing is killed mid-write.
volatile sig_atomic_t g_sigint = 0;
void onSigint(int) { g_sigint = 1; }

void emitRaw(const QString& s) {
    const QByteArray b = s.toUtf8();
    std::fwrite(b.constData(), 1, size_t(b.size()), stdout);
    std::fflush(stdout);
}

QString dim(const QString& s) {
    return QString::fromUtf8(ansi::kDim) + s + QString::fromUtf8(ansi::kReset);
}
QString cyan(const QString& s) {
    return QString::fromUtf8(ansi::kCyan) + s + QString::fromUtf8(ansi::kReset);
}
QString yellow(const QString& s) {
    return QString::fromUtf8(ansi::kYellow) + s + QString::fromUtf8(ansi::kReset);
}
QString green(const QString& s) {
    return QString::fromUtf8(ansi::kGreen) + s + QString::fromUtf8(ansi::kReset);
}

// ---------------------------------------------------------------------------
// @file mentions (port of src/71-mentions.php)
// ---------------------------------------------------------------------------

constexpr int kMentionMaxBytes = 20000;

QStringList mentionPaths(const QString& text) {
    // (?<![\w@]) skips emails (user@host) and @@ — a bare "@" is not a path either.
    static const QRegularExpression re(QStringLiteral("(?<![\\w@])@([^\\s@]+)"));
    QStringList out;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        QString tok = it.next().captured(1);
        while (!tok.isEmpty() && QStringLiteral(".,;:!?)'\"").contains(tok.back()))
            tok.chop(1);
        if (!tok.isEmpty() && !out.contains(tok)) out << tok;
    }
    return out;
}

QString inlineMention(const QString& path) {
    const QFileInfo fi(path);
    if (fi.isDir()) return QStringLiteral("\n[@%1 is a directory — not inlined]\n").arg(path);
    if (!fi.isFile()) return QStringLiteral("\n[@%1 not found]\n").arg(path);

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QStringLiteral("\n[@%1 could not be read]\n").arg(path);
    QByteArray data = f.read(kMentionMaxBytes + 1);
    const qint64 total = fi.size();
    f.close();

    QString note;
    if (data.size() > kMentionMaxBytes) {
        data.truncate(kMentionMaxBytes);
        note = QStringLiteral("\n…[truncated %1 bytes]").arg(total - kMentionMaxBytes);
    }
    return QStringLiteral("\n--- contents of %1 ---\n%2%3\n--- end of %1 ---\n")
        .arg(path, QString::fromUtf8(data), note);
}

QString expandMentions(const QString& text) {
    const QStringList paths = mentionPaths(text);
    if (paths.isEmpty()) return text;
    QString blocks;
    for (const QString& p : paths) blocks += inlineMention(p);
    if (blocks.isEmpty()) return text;
    return text.trimmed() + QLatin1Char('\n') + blocks;
}

// ---------------------------------------------------------------------------
// Undo checkpoints
// ---------------------------------------------------------------------------

QString checkpointsDir() {
    return Config::dataDir() + QStringLiteral("/checkpoints");
}

// The paths a mutating tool is about to touch. bash is deliberately absent: we
// cannot know what a shell command will write, and pretending we can would make
// /undo lie.
QStringList mutatedPaths(const ToolCall& c) {
    QStringList out;
    for (const QString& key : {QStringLiteral("file_path"), QStringLiteral("path"),
                               QStringLiteral("dst")}) {
        const QString v = c.args.value(key).toString();
        if (!v.isEmpty()) out << v;
    }
    return out;
}

QString describeArgs(const ToolCall& c) {
    const QStringList keys{QStringLiteral("file_path"), QStringLiteral("path"),
                           QStringLiteral("pattern"),   QStringLiteral("command"),
                           QStringLiteral("src"),       QStringLiteral("dst")};
    for (const QString& k : keys) {
        const QString v = c.args.value(k).toString();
        if (!v.isEmpty()) return Tui::truncate(v.simplified(), 60);
    }
    return {};
}

QString firstLine(const QString& s, int cols) {
    const QString one = s.section(QLatin1Char('\n'), 0, 0).simplified();
    return Tui::truncate(one, cols);
}

QString humanBytes(qint64 b) {
    if (b >= 1073741824) return QStringLiteral("%1 GB").arg(double(b) / 1073741824.0, 0, 'f', 1);
    if (b >= 1048576) return QStringLiteral("%1 MB").arg(double(b) / 1048576.0, 0, 'f', 1);
    return QStringLiteral("%1 KB").arg(double(b) / 1024.0, 0, 'f', 0);
}

QString relativeTime(qint64 unixSecs) {
    if (unixSecs <= 0) return QStringLiteral("unknown");
    const qint64 d = QDateTime::currentSecsSinceEpoch() - unixSecs;
    if (d < 60) return QStringLiteral("just now");
    if (d < 3600) return QStringLiteral("%1m ago").arg(d / 60);
    if (d < 86400) return QStringLiteral("%1h ago").arg(d / 3600);
    return QStringLiteral("%1d ago").arg(d / 86400);
}

const char* kBanner =
    "  ___  _ _                       ____\n"
    " / _ \\| | | __ _ _ __ ___   __ _|  _ \\  _____   __\n"
    "| | | | | |/ _` | '_ ` _ \\ / _` | | | |/ _ \\ \\ / /\n"
    "| |_| | | | (_| | | | | | | (_| | |_| |  __/\\ V /\n"
    " \\___/|_|_|\\__,_|_| |_| |_|\\__,_|____/ \\___| \\_/\n";

const char* kChatPrompt =
    "You are OllamaDev, a helpful AI coding assistant running locally via Ollama.\n\n"
    "CHAT MODE: you have no tools in this turn. Answer from the conversation itself. "
    "If the user needs a file read or changed, say so and suggest /agent.";

const QStringList& slashCommands() {
    static const QStringList cmds{
        QStringLiteral("/help"),    QStringLiteral("/models"),  QStringLiteral("/model"),
        QStringLiteral("/new"),     QStringLiteral("/exit"),    QStringLiteral("/quit"),
        QStringLiteral("/clear"),   QStringLiteral("/compact"), QStringLiteral("/save"),
        QStringLiteral("/session"), QStringLiteral("/tools"),   QStringLiteral("/context"),
        QStringLiteral("/status"),  QStringLiteral("/pwd"),     QStringLiteral("/cd"),
        QStringLiteral("/ls"),      QStringLiteral("/permission"), QStringLiteral("/undo"),
        QStringLiteral("/init"),    QStringLiteral("/crew"),    QStringLiteral("/retry"),
        QStringLiteral("/plan"),    QStringLiteral("/agent"),   QStringLiteral("/chat"),
        QStringLiteral("/image"),   QStringLiteral("/output-style")};
    return cmds;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Repl::Repl(ReplOptions opt) : opt_(std::move(opt)) {
    backendId_ = opt_.backend.isEmpty()
                     ? Config::str(QStringLiteral("model.backend"), QStringLiteral("ollama"))
                     : opt_.backend;

    std::optional<Session> loaded;
    if (!opt_.sessionId.isEmpty())
        loaded = Session::load(opt_.sessionId);
    else if (opt_.resume)
        loaded = Session::latestForCwd(QDir::currentPath());

    session_ = loaded ? *loaded : Session::create(QDir::currentPath());
    if (!loaded) opt_.resume = false;  // nothing to resume: this is a fresh start

    // Precedence: an explicit -m wins over the resumed session's model, which wins
    // over the backend's default. A `-m` that did NOT take effect on a resume was a
    // real bug in the PHP app (the desktop passes one per terminal).
    if (!opt_.model.isEmpty())
        model_ = opt_.model;
    else if (!session_.model().isEmpty() && (!loaded || session_.backend() == backendId_))
        model_ = session_.model();
    if (model_.isEmpty()) {
        if (const BackendPtr be = Backends::get(backendId_)) model_ = be->defaultModel();
    }

    session_.setModel(model_);
    session_.setBackend(backendId_);

    agent_ = std::make_unique<Agent>(backendId_, model_);
    rebuildPrompts();

    chatOnly_ = opt_.chat;  // `ollamadev chat`: start tool-free, same as /chat
}

Repl::~Repl() = default;

void Repl::rebuildPrompts() {
    agentPrompt_ = agent_->buildSystemPrompt(QDir::currentPath());
    // The chosen output style tunes HOW the model writes; it applies to both the
    // tool-using and the chat prompts.
    const QString styleSuffix = OutputStyles::suffix(OutputStyles::current());
    agentPrompt_ += styleSuffix;
    agent_->setSystemPrompt(agentPrompt_);
    chatPrompt_ = QString::fromUtf8(kChatPrompt) + styleSuffix;
}

// Chat mode must not carry the tool instructions (they tell the model to call
// tools it does not have this turn), so the system message is swapped, not kept.
void Repl::syncSystemMessage() {
    const QString want = chatOnly_ ? chatPrompt_ : agentPrompt_;
    QVector<ChatMessage>& msgs = session_.messages();
    if (!msgs.isEmpty() && msgs.first().role == QLatin1String("system")) {
        msgs.first().content = want;
        return;
    }
    ChatMessage sys;
    sys.role = QStringLiteral("system");
    sys.content = want;
    msgs.prepend(sys);
}

// ---------------------------------------------------------------------------
// Chrome
// ---------------------------------------------------------------------------

QString Repl::shortCwd() const {
    QString cwd = QDir::currentPath();
    const QString home = QDir::homePath();
    if (!home.isEmpty() && cwd.startsWith(home))
        cwd = QStringLiteral("~") + cwd.mid(home.size());
    return cwd;
}

QStringList Repl::installedModels() const {
    if (!modelCache_.isEmpty()) return modelCache_;
    if (const BackendPtr be = Backends::get(backendId_)) modelCache_ = be->models();
    return modelCache_;
}

void Repl::banner() const {
    if (!qEnvironmentVariableIsEmpty("OLLAMADEV_NO_BANNER")) return;
    emitRaw(QLatin1Char('\n') + cyan(QString::fromUtf8(ansi::kBold) + QString::fromUtf8(kBanner)) +
            QLatin1Char('\n'));
    emitRaw(dim(QStringLiteral("  OllamaDev ") + QStringLiteral(ODV_VERSION) +
                QStringLiteral(" · local AI coding assistant")) +
            QStringLiteral("\n\n"));
    emitRaw(QStringLiteral("  ") + dim(QStringLiteral("model")) + QLatin1Char(' ') +
            cyan(model_) + QStringLiteral("   ") +
            dim(QStringLiteral("· ") + backendId_ + QStringLiteral(" · ") +
                (chatOnly_ ? QStringLiteral("chat") : QStringLiteral("agent")) +
                QStringLiteral(" mode · ") + Permission::modeName(Permission::mode())) +
            QLatin1Char('\n'));
    emitRaw(QStringLiteral("  ") +
            dim(QStringLiteral("/help for commands · @file inlines a file · Ctrl-C interrupts a "
                               "response")) +
            QStringLiteral("\n\n"));
}

bool Repl::preflight() const {
    const BackendPtr be = Backends::get(backendId_);
    if (!be) {
        emitRaw(yellow(QStringLiteral("  ⚠ unknown backend: ")) + backendId_ + QLatin1Char('\n'));
        return false;
    }
    if (!be->available()) {
        emitRaw(QLatin1Char('\n') + yellow(QStringLiteral("  ⚠ can't reach ")) +
                Backends::labelFor(backendId_) + QLatin1Char('\n'));
        if (backendId_ == QLatin1String("ollama")) {
            emitRaw(dim(QStringLiteral("    1. start the server:  ")) +
                    cyan(QStringLiteral("ollama serve")) + QLatin1Char('\n'));
            emitRaw(dim(QStringLiteral("    2. pull a model:      ")) +
                    cyan(QStringLiteral("ollama pull qwen3.5:9b")) + QLatin1Char('\n'));
            emitRaw(dim(QStringLiteral("    remote host? set OLLAMA_HOST or ollama.host in the "
                                       "config.")) +
                    QStringLiteral("\n\n"));
        }
        return false;
    }
    if (installedModels().isEmpty()) {
        emitRaw(QLatin1Char('\n') +
                yellow(QStringLiteral("  ⚠ the backend is up but has no models installed.")) +
                QLatin1Char('\n') + dim(QStringLiteral("    pull one:  ")) +
                cyan(QStringLiteral("ollama pull qwen3.5:9b")) + QStringLiteral("\n\n"));
        return false;
    }
    return true;
}

void Repl::resumeNotice() const {
    const QVector<ChatMessage>& msgs = session_.messages();
    int real = 0;
    for (const ChatMessage& m : msgs)
        if (m.role != QLatin1String("system")) ++real;
    if (real == 0) return;

    emitRaw(dim(QStringLiteral("  ↻ resumed session · %1 messages").arg(real)) + QLatin1Char('\n'));

    // Show the last exchange: a resume that prints nothing back feels like it did
    // not happen, and you cannot see what context you are continuing from.
    QString lastUser, lastAsst;
    for (int i = msgs.size() - 1; i >= 0; --i) {
        const ChatMessage& m = msgs.at(i);
        const QString body = m.content.simplified();
        if (body.isEmpty()) continue;
        if (lastAsst.isEmpty() && m.role == QLatin1String("assistant")) lastAsst = body;
        else if (lastUser.isEmpty() && m.role == QLatin1String("user")) lastUser = body;
        if (!lastUser.isEmpty() && !lastAsst.isEmpty()) break;
    }
    if (!lastUser.isEmpty())
        emitRaw(dim(QStringLiteral("    you ▸ ") + Tui::truncate(lastUser, 70)) + QLatin1Char('\n'));
    if (!lastAsst.isEmpty())
        emitRaw(dim(QStringLiteral("    %1 ▸ ").arg(model_) + Tui::truncate(lastAsst, 70)) +
                QLatin1Char('\n'));
    emitRaw(dim(QStringLiteral("    /new for a fresh session")) + QStringLiteral("\n\n"));
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

std::optional<QString> Repl::readRawLine() {
    // Unbuffered on purpose: the line editor reads the same fd with read(2), and a
    // buffered stream could swallow bytes it then never gives back.
    QByteArray line;
    char c = 0;
    while (true) {
        const ssize_t n = ::read(STDIN_FILENO, &c, 1);
        if (n <= 0) return line.isEmpty() ? std::nullopt : std::optional<QString>(QString::fromUtf8(line));
        if (c == '\n') break;
        line.append(c);
    }
    if (line.endsWith('\r')) line.chop(1);
    return QString::fromUtf8(line);
}

std::optional<QString> Repl::readInput() {
    if (LineEditor::supported()) {
        LineEditor::Status st;
        st.model = model_;
        st.mode = chatOnly_ ? QStringLiteral("chat") : Permission::modeName(Permission::mode());
        st.cwd = shortCwd();
        return LineEditor::readLine(st, history_,
                                    [this](const QString& l, int c) { return complete(l, c); });
    }
    // Pipes, daemons and the embedded ADE terminal: plain line input, no cursor
    // control (the host echoes the keystrokes itself).
    emitRaw(QLatin1Char('\n') +
            dim(model_ + QStringLiteral("  ·  ") +
                (chatOnly_ ? QStringLiteral("chat") : QStringLiteral("agent")) +
                QStringLiteral("  ·  ") + shortCwd()) +
            QLatin1Char('\n') + cyan(QStringLiteral("❯ ")));
    return readRawLine();
}

bool Repl::confirm(const QString& question) const {
    if (!Permission::interactive()) return false;
    emitRaw(question + QLatin1Char(' '));
    const auto line = readRawLine();
    if (!line) return false;
    const QString a = line->trimmed().toLower();
    return a == QLatin1String("y") || a == QLatin1String("yes");
}

QStringList Repl::pathCandidates(const QString& token) const {
    QString parent = QStringLiteral(".");
    QString prefix;
    const int slash = token.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0) {
        parent = slash == 0 ? QStringLiteral("/") : token.left(slash);
        prefix = token.left(slash + 1);
    }
    QDir d(parent);
    if (!d.exists()) return {};
    const QString needle = token.mid(slash + 1);

    QStringList out;
    for (const QFileInfo& fi : d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
        if (!needle.isEmpty() && !fi.fileName().startsWith(needle)) continue;
        out << prefix + fi.fileName() + (fi.isDir() ? QStringLiteral("/") : QString());
    }
    out.sort();
    return out;
}

LineEditor::Completion Repl::complete(const QString& line, int cursor) const {
    LineEditor::Completion out;
    const QVector<QString> g = Tui::glyphs(line);
    const int cur = qBound(0, cursor, int(g.size()));

    QString before;
    for (int i = 0; i < cur; ++i) before += g.at(i);

    int tokenStart = cur;
    while (tokenStart > 0 && !g.at(tokenStart - 1).at(0).isSpace()) --tokenStart;
    QString token;
    for (int i = tokenStart; i < cur; ++i) token += g.at(i);
    out.start = tokenStart;

    // @path mentions complete like paths, with the @ kept.
    if (token.startsWith(QLatin1Char('@'))) {
        for (const QString& p : pathCandidates(token.mid(1)))
            out.candidates << QLatin1Char('@') + p;
        return out;
    }

    const QStringList words = before.split(QRegularExpression(QStringLiteral("\\s+")),
                                           Qt::SkipEmptyParts);
    const bool firstWord = words.size() <= 1 && !before.endsWith(QLatin1Char(' '));

    if (firstWord) {
        for (const QString& c : slashCommands())
            if (token.isEmpty() || c.startsWith(token)) out.candidates << c;
        return out;
    }

    const QString cmd = words.first().startsWith(QLatin1Char('/')) ? words.first().mid(1)
                                                                   : words.first();
    if (cmd == QLatin1String("model")) {
        for (const QString& m : installedModels())
            if (token.isEmpty() || m.startsWith(token)) out.candidates << m;
    } else {
        out.candidates = pathCandidates(token);
    }
    out.candidates.sort();
    return out;
}

// ---------------------------------------------------------------------------
// Undo
// ---------------------------------------------------------------------------

void Repl::snapshot(const QVector<ToolCall>& calls) {
    for (const ToolCall& c : calls) {
        const ToolDef* def = Tools::find(c.name);
        if (!def || !def->mutates) continue;
        for (const QString& raw : mutatedPaths(c)) {
            bool ok = false;
            const QString path = Tools::resolvePath(raw, &ok);
            if (!ok || path.isEmpty()) continue;
            if (!touchedThisTurn_.contains(path)) touchedThisTurn_ << path;

            QDir().mkpath(checkpointsDir());
            const QFileInfo fi(path);
            QJsonObject ck{{"path", path},
                           {"existed", fi.isFile()},
                           {"tool", c.name},
                           {"ts", double(QDateTime::currentSecsSinceEpoch())}};
            if (fi.isFile()) {
                // A snapshot we cannot hold in memory is worse than no snapshot: it
                // would make /undo claim a revert it cannot perform.
                if (fi.size() > 5 * 1024 * 1024) continue;
                QFile f(path);
                if (!f.open(QIODevice::ReadOnly)) continue;
                ck.insert(QStringLiteral("content"), QString::fromLatin1(f.readAll().toBase64()));
                f.close();
            }
            const QString name =
                QStringLiteral("%1/ck_%2_%3.json")
                    .arg(checkpointsDir())
                    .arg(QDateTime::currentMSecsSinceEpoch(), 14, 10, QLatin1Char('0'))
                    .arg(qHash(path), 8, 16, QLatin1Char('0'));
            QFile out(name);
            if (!out.open(QIODevice::WriteOnly)) continue;
            out.write(QJsonDocument(ck).toJson(QJsonDocument::Compact));
            out.close();
        }
    }
}

QString Repl::undoLast() {
    QDir d(checkpointsDir());
    const QStringList files = d.entryList({QStringLiteral("ck_*.json")}, QDir::Files, QDir::Name);
    if (files.isEmpty()) return dim(QStringLiteral("  nothing to undo\n"));

    const QString path = d.filePath(files.last());
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return dim(QStringLiteral("  checkpoint unreadable\n"));
    const QJsonObject ck = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    QFile::remove(path);

    const QString target = ck.value(QStringLiteral("path")).toString();
    if (target.isEmpty()) return dim(QStringLiteral("  checkpoint had no path\n"));

    if (!ck.value(QStringLiteral("existed")).toBool()) {
        QFile::remove(target);
        return green(QStringLiteral("  ✓ removed ")) + target +
               dim(QStringLiteral(" (it did not exist before)\n"));
    }
    QFile out(target);
    if (!out.open(QIODevice::WriteOnly))
        return yellow(QStringLiteral("  ✗ could not restore ")) + target + QLatin1Char('\n');
    out.write(QByteArray::fromBase64(
        ck.value(QStringLiteral("content")).toString().toLatin1()));
    out.close();
    return green(QStringLiteral("  ✓ reverted ")) + target + QLatin1Char('\n');
}

// ---------------------------------------------------------------------------
// One turn
// ---------------------------------------------------------------------------

void Repl::announceCalls(const QVector<ToolCall>& calls) const {
    for (const ToolCall& c : calls) {
        const QString args = describeArgs(c);
        emitRaw(dim(QStringLiteral("  ⚙ ") + c.name +
                    (args.isEmpty() ? QString() : QStringLiteral("(") + args + QStringLiteral(")"))) +
                QLatin1Char('\n'));
    }
}

void Repl::reportResults(int firstNewMessage) const {
    const QVector<ChatMessage>& msgs = session_.messages();
    for (int i = firstNewMessage; i < msgs.size(); ++i) {
        const ChatMessage& m = msgs.at(i);
        if (m.role != QLatin1String("tool")) continue;
        emitRaw(dim(QStringLiteral("    ↳ ") + firstLine(m.content, qMax(20, Tui::width() - 8))) +
                QLatin1Char('\n'));
    }
}

// Reprint the streamed answer with markdown styling. Only safe when every line of
// it fits the terminal: the cursor-up count is a count of LINE BREAKS, and a line
// the terminal soft-wrapped occupies two rows, so the climb would land in the
// middle of the answer and duplicate half of it.
void Repl::restyleAnswer(const QString& answer) const {
    if (!Render::enabled()) return;
    if (!qEnvironmentVariableIsEmpty("OLLAMADEV_SIMPLE_INPUT")) return;

    const QString trimmed = answer.trimmed();
    if (trimmed.isEmpty()) return;

    const int w = Tui::width();
    const QStringList lines = trimmed.split(QLatin1Char('\n'));
    for (const QString& l : lines)
        if (Tui::visWidth(l) >= w) return;  // it wrapped; the row math cannot be trusted

    const QString styled = Render::markdown(trimmed);
    if (styled == trimmed) return;

    const int back = lines.size() - 1;  // the cursor already sits on the last line
    QString seq;
    if (back > 0) seq += QStringLiteral("\033[%1A").arg(back);
    seq += QStringLiteral("\r\033[J") + styled;
    emitRaw(seq);
}

void Repl::runTurn(const QString& text) {
    const BackendPtr be = Backends::get(backendId_);
    if (!be) {
        emitRaw(yellow(QStringLiteral("  unknown backend\n")));
        return;
    }

    syncSystemMessage();
    ChatMessage user;
    user.role = QStringLiteral("user");

    // Vision: pull any /image or @image.png attachments out FIRST, base64 them onto
    // the message's images[] (Ollama sends them to a vision model, and ignores them
    // on a text one), and strip the tokens so what's left is a natural prompt. This
    // runs before expandMentions so an image @token is never inlined as raw bytes.
    const QStringList imgPaths = Vision::extractImagePaths(text);
    for (const QString& p : imgPaths) {
        const QString b64 = Vision::encodeBase64(p);
        if (!b64.isEmpty()) user.images << b64;
    }
    if (!user.images.isEmpty())
        emitRaw(dim(QStringLiteral("  🖼 attached %1 image(s)").arg(user.images.size())) +
                QLatin1Char('\n'));
    const QString cleaned = imgPaths.isEmpty() ? text : Vision::stripImageTokens(text);

    user.content = expandMentions(cleaned);
    session_.messages().append(user);
    session_.save();

    touchedThisTurn_.clear();
    interrupted_ = false;
    g_sigint = 0;

    CancelToken cancel;
    // The backend polls the token on its own timer inside its nested event loop,
    // so this timer (started before the call) fires there too and turns a signal
    // into a cooperative cancel. No thread is ever killed.
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, [&] {
        if (g_sigint && !cancel.cancelled()) {
            cancel.cancel();
            interrupted_ = true;
        }
    });
    poll.start(50);

    const bool control = Tui::stdoutIsTty();
    const int window = Config::integer(QStringLiteral("ui.thinkingRows"), 6);

    std::unique_ptr<Thinking> box;
    bool answered = false;
    QString answer;

    StreamSink sink;
    sink.onThinking = [&](const QString& t) {
        if (box) box->push(t);
    };
    sink.onContent = [&](const QString& c) {
        if (c.isEmpty()) return;
        if (!answered) {
            if (box) box->collapse();  // the answer has started: fold the reasoning away
            answered = true;
        }
        answer += c;
        emitRaw(c);
    };

    const int maxIter = qMax(1, Config::integer(QStringLiteral("agents.maxIterations"), 12));
    for (int i = 0; i < maxIter; ++i) {
        if (cancel.cancelled()) break;

        answered = false;
        answer.clear();
        Thinking::Options o;
        o.window = window;
        o.control = control;
        box = std::make_unique<Thinking>([](const QString& s) { emitRaw(s); }, o);

        ChatTurn t;
        if (chatOnly_ || !be->supportsNativeTools()) {
            // Chat mode: no schemas, and the assistant message is appended here
            // because we bypassed Agent::turn to get it.
            t = be->chat(model_, session_.messages(), QJsonArray(), sink, cancel);
            if (t.ok) {
                ChatMessage m;
                m.role = QStringLiteral("assistant");
                m.content = t.content;
                m.thinking = t.thinking;
                session_.messages().append(m);
                // Agent::turn meters usage itself; this branch bypasses it, so
                // record the chat/CLI turn's tokens here to keep `stats` complete.
                if (t.promptTokens > 0 || t.evalTokens > 0)
                    Usage::record(model_, t.promptTokens, t.evalTokens);
            }
        } else {
            t = agent_->turn(session_.messages(), sink, cancel);
        }
        box->collapse();

        if (!t.ok) {
            if (!cancel.cancelled())
                emitRaw(QLatin1Char('\n') + yellow(QStringLiteral("  ✗ ") +
                                                   (t.error.isEmpty()
                                                        ? QStringLiteral("the model turn failed")
                                                        : t.error)) +
                        QLatin1Char('\n'));
            break;
        }

        if (t.calls.isEmpty()) {
            restyleAnswer(answer);
            break;
        }

        emitRaw(QStringLiteral("\n"));
        announceCalls(t.calls);
        snapshot(t.calls);  // so /undo can put back whatever the next line overwrites
        const int mark = session_.messages().size();
        agent_->executeCalls(t.calls, session_.messages());
        reportResults(mark);
        session_.save();
    }

    poll.stop();
    emitRaw(QStringLiteral("\n"));
    if (interrupted_) emitRaw(dim(QStringLiteral("  interrupted\n")));

    QStringList existing;
    for (const QString& p : touchedThisTurn_)
        existing << QDir::current().relativeFilePath(p);
    if (!existing.isEmpty())
        emitRaw(dim(QStringLiteral("  ✎ ") + existing.join(QStringLiteral(", ")) +
                    QStringLiteral("  (/undo reverts the last one)")) +
                QLatin1Char('\n'));

    session_.save();

    // Auto-compaction: bound the transcript before it overflows num_ctx, not after.
    const int threshold = Config::integer(QStringLiteral("agents.compactThreshold"), 30);
    if (session_.messages().size() >= threshold) {
        const int keep = Config::integer(QStringLiteral("agents.compactKeep"), 8);
        emitRaw(dim(QStringLiteral("  📝 compacting the conversation…")) + QLatin1Char('\n'));
        session_.compact(keep);
    }
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

QString Repl::cmdHelp() const {
    auto row = [](const QString& cmd, const QString& desc) {
        return QStringLiteral("  ") + cyan(cmd.leftJustified(22)) + dim(desc) + QLatin1Char('\n');
    };
    QString o = QLatin1Char('\n') + QString::fromUtf8(ansi::kBold) + QStringLiteral("  Commands") +
                QString::fromUtf8(ansi::kReset) + QStringLiteral("\n\n");
    o += dim(QStringLiteral("  Conversation")) + QLatin1Char('\n');
    o += row(QStringLiteral("/chat · /agent"), QStringLiteral("chat-only vs. tool-using mode"));
    o += row(QStringLiteral("/retry"), QStringLiteral("re-run the last turn"));
    o += row(QStringLiteral("/compact"), QStringLiteral("summarise older messages to free context"));
    o += row(QStringLiteral("/new"), QStringLiteral("start a fresh session"));
    o += QLatin1Char('\n') + dim(QStringLiteral("  Models")) + QLatin1Char('\n');
    o += row(QStringLiteral("/model <name>"), QStringLiteral("switch model"));
    o += row(QStringLiteral("/models"), QStringLiteral("list installed models"));
    o += QLatin1Char('\n') + dim(QStringLiteral("  Files & edits")) + QLatin1Char('\n');
    o += row(QStringLiteral("@path/to/file"), QStringLiteral("inline a file into your message"));
    o += row(QStringLiteral("/image <path>"), QStringLiteral("attach an image (or @pic.png) for a vision model"));
    o += row(QStringLiteral("/undo"), QStringLiteral("revert the most recent file edit"));
    o += row(QStringLiteral("/init"), QStringLiteral("generate OLLAMADEV.md project memory"));
    o += QLatin1Char('\n') + dim(QStringLiteral("  Project & context")) + QLatin1Char('\n');
    o += row(QStringLiteral("/context · /status"), QStringLiteral("context fill, model, hardware"));
    o += row(QStringLiteral("/tools"), QStringLiteral("list available tools"));
    o += row(QStringLiteral("/permission [mode]"), QStringLiteral("auto | ask | readonly | plan"));
    o += row(QStringLiteral("/output-style"), QStringLiteral("default | concise | explanatory | formal | bullets"));
    o += row(QStringLiteral("/plan [on|off]"),
             QStringLiteral("research read-only, propose a plan, edit on approval"));
    o += row(QStringLiteral("/crew <task>"),
             QStringLiteral("the parallel bench (research → plan → N coders → audit)"));
    o += QLatin1Char('\n') + dim(QStringLiteral("  Session")) + QLatin1Char('\n');
    o += row(QStringLiteral("/cd · /ls · /pwd"), QStringLiteral("navigate the working directory"));
    o += row(QStringLiteral("/save · /session"), QStringLiteral("save / show the current session"));
    o += row(QStringLiteral("/clear · /exit"), QStringLiteral("clear the screen / quit"));
    o += QLatin1Char('\n') +
         dim(QStringLiteral("  Tab completes commands, paths and model names. Ctrl-C interrupts a "
                            "response.")) +
         QLatin1Char('\n');
    return o;
}

QString Repl::cmdModels() const {
    const QStringList ms = installedModels();
    if (ms.isEmpty()) return dim(QStringLiteral("  no models installed\n"));
    QString o = QStringLiteral("\n");
    for (const QString& m : ms) {
        const bool cur = m == model_;
        o += QStringLiteral("  ") + (cur ? green(QStringLiteral("●")) : dim(QStringLiteral("○"))) +
             QLatin1Char(' ') + (cur ? cyan(m) : m) +
             (Models::isCloud(m) ? dim(QStringLiteral("  cloud")) : QString()) + QLatin1Char('\n');
    }
    return o + QLatin1Char('\n');
}

QString Repl::cmdModel(const QString& name) {
    const QString want = name.trimmed();
    if (want.isEmpty()) return QStringLiteral("  current: ") + cyan(model_) + QLatin1Char('\n');

    const QStringList installed = installedModels();
    const QString resolved = Models::match(want, installed);
    if (resolved.isEmpty()) {
        return yellow(QStringLiteral("  '%1' is not installed.").arg(want)) + QLatin1Char('\n') +
               dim(QStringLiteral("  installed: ") + installed.join(QStringLiteral(", "))) +
               QLatin1Char('\n') + dim(QStringLiteral("  pull it with: ")) +
               cyan(QStringLiteral("ollama pull ") + Models::resolveTag(want)) + QLatin1Char('\n');
    }

    model_ = resolved;
    session_.setModel(model_);
    session_.save();
    agent_ = std::make_unique<Agent>(backendId_, model_);
    rebuildPrompts();
    return green(QStringLiteral("  ✓ switched to ")) + cyan(resolved) + QLatin1Char('\n');
}

int Repl::estimateTokens() const {
    int chars = 0;
    for (const ChatMessage& m : session_.messages()) chars += m.content.size();
    return chars / 4;  // the usual rough rule; Ollama's real count only exists post-turn
}

QString Repl::cmdContext() const {
    const int used = estimateTokens();
    const int window = Config::integer(QStringLiteral("ollama.contextWindow"), 16384);
    const int pct = window > 0 ? qBound(0, used * 100 / window, 100) : 0;
    const int bars = 20;
    const int on = pct * bars / 100;

    QString o = QStringLiteral("📁 ") + QDir::currentPath() + QLatin1Char('\n');
    o += QStringLiteral("Context: [") + QString(on, QLatin1Char('=')) +
         QString(bars - on, QLatin1Char(' ')) +
         QStringLiteral("] %1%  (~%2 / %3 tokens)\n").arg(pct).arg(used).arg(window);
    o += QStringLiteral("Messages: %1 | Model: %2 | Backend: %3\n")
             .arg(session_.messages().size())
             .arg(model_, backendId_);

    // How the model is actually loaded. "It's slow" is nearly always a CPU spill,
    // and this is the only place that says so out loud.
    if (const auto ollama = std::dynamic_pointer_cast<OllamaBackend>(Backends::get(backendId_))) {
        const QJsonObject ps = ollama->psInfo(model_);
        if (!ps.isEmpty()) {
            const int gpu = ps.value(QStringLiteral("gpuPct")).toInt();
            const qint64 vram = qint64(ps.value(QStringLiteral("vram")).toDouble());
            QString load;
            if (gpu >= 100)
                load = cyan(QStringLiteral("100% GPU")) + dim(QStringLiteral(" · ") + humanBytes(vram) + QStringLiteral(" VRAM"));
            else if (gpu <= 0)
                load = yellow(QStringLiteral("100% CPU")) + dim(QStringLiteral(" · no GPU offload (slow)"));
            else
                load = yellow(QStringLiteral("%1% GPU / %2% CPU").arg(gpu).arg(100 - gpu)) +
                       dim(QStringLiteral(" · ") + humanBytes(vram) + QStringLiteral(" VRAM · spilling to CPU"));
            o += QStringLiteral("Hardware: ") + load + QLatin1Char('\n');
        } else {
            o += QStringLiteral("Hardware: ") +
                 dim(QStringLiteral("not loaded yet (run a prompt, then /context)")) +
                 QLatin1Char('\n');
        }
    }
    return o;
}

QString Repl::cmdPermission(const QString& args) {
    const QString a = args.trimmed().toLower();
    if (a.isEmpty() || a == QLatin1String("status")) {
        QString o = QStringLiteral("Permission mode: ") + cyan(Permission::modeName(Permission::mode())) +
                    QLatin1Char('\n');
        o += dim(QStringLiteral("  auto     - run every tool without asking\n"
                                "  ask      - prompt before a mutating tool (default)\n"
                                "  readonly - block every mutating tool\n"
                                "  plan     - research only; edit after you approve a plan (/plan)\n"));
        return o;
    }
    // `/permission mode ask` and `/permission ask` both work: the PHP form and the
    // one people actually type.
    const QString want = a.startsWith(QLatin1String("mode "))
                             ? a.mid(5).trimmed()
                             : a;
    static const QStringList known{QStringLiteral("auto"), QStringLiteral("ask"),
                                   QStringLiteral("readonly"), QStringLiteral("plan")};
    if (!known.contains(want))
        return yellow(QStringLiteral("  usage: /permission [auto|ask|readonly|plan]\n"));

    Permission::setMode(Permission::modeFromName(want));
    rebuildPrompts();  // plan mode changes the system prompt, not just the gate
    return QStringLiteral("Permission mode: ") + cyan(Permission::modeName(Permission::mode())) +
           QLatin1Char('\n');
}

QString Repl::cmdPlan(const QString& args) {
    const QString a = args.trimmed().toLower();
    const bool wasPlan = Permission::mode() == PermMode::Plan;
    const bool on = a == QLatin1String("on") || (!wasPlan && a != QLatin1String("off"));

    if (on) Permission::setMode(PermMode::Plan);
    else if (wasPlan) Permission::exitPlan();  // back to whatever preceded plan, not a guessed default

    rebuildPrompts();
    return on ? cyan(QStringLiteral("  📋 plan mode ON")) +
                    dim(QStringLiteral(" — read-only research; I'll propose a plan and wait for "
                                       "your approval before editing.\n"))
              : cyan(QStringLiteral("  plan mode OFF")) +
                    dim(QStringLiteral(" — back to ") + Permission::modeName(Permission::mode()) +
                        QStringLiteral(" mode.\n"));
}

QString Repl::cmdCrew(const QString& args) {
    QString task = args.trimmed();
    if (task.isEmpty())
        return QStringLiteral("Usage: /crew <task> [--max N] [--review] [--no-research] "
                              "[--no-audit] [--focus \"…\"]\n");

    CrewOptions o;
    o.maxCoders = Config::integer(QStringLiteral("crew.maxCoders"), 4);
    o.land = Config::str(QStringLiteral("crew.land"), QStringLiteral("auto"));

    static const QRegularExpression maxRe(QStringLiteral("\\s--max\\s+(\\d+)"));
    if (const auto m = maxRe.match(task); m.hasMatch()) {
        o.maxCoders = m.captured(1).toInt();
        task.remove(m.capturedStart(), m.capturedLength());
    }
    static const QRegularExpression focusRe(QStringLiteral("\\s--focus\\s+\"([^\"]*)\"|\\s--focus\\s+(\\S+)"));
    if (const auto m = focusRe.match(task); m.hasMatch()) {
        o.focus = m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
        task.remove(m.capturedStart(), m.capturedLength());
    }
    auto takeFlag = [&task](const QString& flag) {
        const QRegularExpression re(QStringLiteral("(^|\\s)") + QRegularExpression::escape(flag) +
                                    QStringLiteral("(\\s|$)"));
        const auto m = re.match(task);
        if (!m.hasMatch()) return false;
        task.remove(m.capturedStart(), m.capturedLength());
        return true;
    };
    if (takeFlag(QStringLiteral("--review"))) o.land = QStringLiteral("review");
    if (takeFlag(QStringLiteral("--no-research"))) o.research = false;
    if (takeFlag(QStringLiteral("--no-audit"))) o.audit = false;

    o.task = task.trimmed();
    if (o.task.isEmpty()) return QStringLiteral("Usage: /crew <task>\n");

    CrewEvents ev;
    ev.onPhase = [](const QString& p, const QString& m) {
        emitRaw(QLatin1Char('\n') + cyan(QStringLiteral("▸ ") + p) + QStringLiteral(": ") + m +
                QLatin1Char('\n'));
    };
    ev.onLog = [](const QString& m) { emitRaw(dim(QStringLiteral("  ") + m) + QLatin1Char('\n')); };
    ev.onCoderState = [](int n, const QString& s) {
        emitRaw(dim(QStringLiteral("  coder #%1 → %2").arg(n).arg(s)) + QLatin1Char('\n'));
    };

    CancelToken cancel;
    const Crew::Result r = Crew::run(o, ev, cancel);
    QString out = QStringLiteral("\n  %1 applied · %2 held\n").arg(r.applied.size()).arg(r.held.size());
    if (!r.held.isEmpty())
        out += dim(QStringLiteral("  review: ollamadev board · apply: ollamadev crew accept <n>\n"));
    return out;
}

QString Repl::cmdInit() {
    emitRaw(dim(QStringLiteral("  scanning the project…")) + QLatin1Char('\n'));
    runTurn(QStringLiteral(
        "Explore this project (its layout, build system, entry points and conventions) using your "
        "tools, then WRITE a file called OLLAMADEV.md at the project root. It must be the context a "
        "new contributor needs: what the project is, how to build and test it, the directory map, "
        "the conventions to follow, and anything surprising. Keep it under 100 lines. Write the "
        "file with the write tool — do not paste it into your reply."));
    return {};
}

QString Repl::cmdRetry() {
    QVector<ChatMessage>& msgs = session_.messages();
    // Rewind to just before the last user message, then replay it: the assistant
    // turn and everything the tools appended after it are dropped, so the model
    // answers afresh instead of "regenerating" on top of its own last answer.
    int lastUser = -1;
    for (int i = msgs.size() - 1; i >= 0; --i) {
        if (msgs.at(i).role == QLatin1String("user")) {
            lastUser = i;
            break;
        }
    }
    if (lastUser < 0) return dim(QStringLiteral("  nothing to retry\n"));

    const QString prompt = msgs.at(lastUser).content;
    while (msgs.size() > lastUser) msgs.removeLast();
    session_.save();
    emitRaw(dim(QStringLiteral("  regenerating…")) + QLatin1Char('\n'));
    runTurn(prompt);
    return {};
}

QString Repl::cmdCd(const QString& dir) {
    const QString d = dir.trimmed();
    if (d.isEmpty()) return QDir::currentPath() + QLatin1Char('\n');
    if (!QDir::setCurrent(d)) return yellow(QStringLiteral("  not a directory: ")) + d + QLatin1Char('\n');
    // Tools resolve relative paths against the thread root, not the process cwd
    // (crew coders each have their own sandbox), so it has to move too.
    Tools::setThreadRoot(QDir::currentPath());
    rebuildPrompts();
    return QStringLiteral("📁 ") + QDir::currentPath() + QLatin1Char('\n');
}

QString Repl::cmdLs(const QString& dir) const {
    const ToolResult r = Tools::run(QStringLiteral("ls"),
                                    QJsonObject{{"path", dir.trimmed().isEmpty()
                                                             ? QStringLiteral(".")
                                                             : dir.trimmed()}});
    return (r.ok ? r.output : r.error) + QLatin1Char('\n');
}

QString Repl::cmdOutputStyle(const QString& args) {
    const QString want = args.trimmed().toLower();
    if (!want.isEmpty()) {
        if (!OutputStyles::set(want))
            return yellow(QStringLiteral("  unknown style: %1\n").arg(want)) +
                   dim(QStringLiteral("  choose one of: ") +
                       OutputStyles::names().join(QStringLiteral(", ")) + QLatin1Char('\n'));
        rebuildPrompts();  // the style is a system-prompt suffix
    }
    const QString cur = OutputStyles::current();
    QString o = QLatin1Char('\n') + dim(QStringLiteral("  Output styles:")) + QLatin1Char('\n');
    for (const QString& name : OutputStyles::names()) {
        const bool sel = name == cur;
        o += QStringLiteral("  ") + (sel ? green(QStringLiteral("●")) : QStringLiteral(" ")) +
             QLatin1Char(' ') + (sel ? cyan(name) : name) +
             dim(QStringLiteral(" — ") + OutputStyles::describe(name)) + QLatin1Char('\n');
    }
    o += dim(QStringLiteral("  set: /output-style <name>\n"));
    return o;
}

Repl::Slash Repl::slash(const QString& input) {
    Slash s;
    if (!input.startsWith(QLatin1Char('/'))) return s;

    const QString body = input.mid(1);
    const int sp = body.indexOf(QRegularExpression(QStringLiteral("\\s")));
    const QString cmd = (sp < 0 ? body : body.left(sp)).toLower();
    const QString args = sp < 0 ? QString() : body.mid(sp + 1).trimmed();

    s.handled = true;
    QString out;

    if (cmd == QLatin1String("help")) out = cmdHelp();
    else if (cmd == QLatin1String("models")) out = cmdModels();
    else if (cmd == QLatin1String("model")) out = cmdModel(args);
    else if (cmd == QLatin1String("exit") || cmd == QLatin1String("quit")) {
        session_.save();
        emitRaw(dim(QStringLiteral("  bye\n")));
        s.quit = true;
    } else if (cmd == QLatin1String("new")) {
        session_.save();
        session_ = Session::create(QDir::currentPath());
        session_.setModel(model_);
        session_.setBackend(backendId_);
        history_.clear();
        out = green(QStringLiteral("  ✓ new session ")) + dim(session_.id()) + QLatin1Char('\n');
    } else if (cmd == QLatin1String("clear")) {
        out = QString::fromUtf8(ansi::kClear);
    } else if (cmd == QLatin1String("compact")) {
        const int keep = Config::integer(QStringLiteral("agents.compactKeep"), 8);
        const int before = session_.messages().size();
        emitRaw(dim(QStringLiteral("  📝 compacting…")) + QLatin1Char('\n'));
        session_.compact(keep);
        out = dim(QStringLiteral("  %1 → %2 messages\n")
                      .arg(before)
                      .arg(session_.messages().size()));
    } else if (cmd == QLatin1String("save")) {
        session_.save();
        out = dim(QStringLiteral("  saved ") + session_.id() + QLatin1Char('\n'));
    } else if (cmd == QLatin1String("session")) {
        const SessionMeta m = session_.meta();
        out = QStringLiteral("Session: %1\n  model %2 · backend %3 · %4 messages · %5\n")
                  .arg(m.id, m.model, m.backend)
                  .arg(m.messages)
                  .arg(relativeTime(m.updated));
    } else if (cmd == QLatin1String("tools")) {
        const QStringList names = Tools::names();
        out = QStringLiteral("\nTools (%1):\n  ").arg(names.size()) +
              names.join(QStringLiteral(", ")) + QLatin1Char('\n');
    } else if (cmd == QLatin1String("context") || cmd == QLatin1String("status")) {
        out = cmdContext();
    } else if (cmd == QLatin1String("pwd")) {
        out = QDir::currentPath() + QLatin1Char('\n');
    } else if (cmd == QLatin1String("cd")) {
        out = cmdCd(args);
    } else if (cmd == QLatin1String("ls")) {
        out = cmdLs(args);
    } else if (cmd == QLatin1String("permission") || cmd == QLatin1String("permissions")) {
        out = cmdPermission(args);
    } else if (cmd == QLatin1String("undo")) {
        out = undoLast();
    } else if (cmd == QLatin1String("init")) {
        out = cmdInit();
    } else if (cmd == QLatin1String("crew")) {
        out = cmdCrew(args);
    } else if (cmd == QLatin1String("retry") || cmd == QLatin1String("regenerate")) {
        out = cmdRetry();
    } else if (cmd == QLatin1String("plan")) {
        out = cmdPlan(args);
    } else if (cmd == QLatin1String("agent")) {
        chatOnly_ = false;
        out = cyan(QStringLiteral("  agent mode")) + dim(QStringLiteral(" — tools enabled.\n"));
    } else if (cmd == QLatin1String("chat")) {
        chatOnly_ = args.toLower() != QLatin1String("off");
        out = chatOnly_ ? cyan(QStringLiteral("  chat mode")) +
                              dim(QStringLiteral(" — pure conversation, tools off.\n"))
                        : cyan(QStringLiteral("  agent mode")) +
                              dim(QStringLiteral(" — tools enabled.\n"));
    } else if (cmd == QLatin1String("image")) {
        if (args.isEmpty()) {
            out = dim(QStringLiteral("  usage: /image <path> [prompt]   (also: @pic.png in any "
                                     "message)\n"));
        } else {
            // Hand the whole line to the turn; runTurn's vision path recognises the
            // leading /image form and attaches the picture.
            s.prompt = input;
        }
    } else if (cmd == QLatin1String("output-style") || cmd == QLatin1String("style")) {
        out = cmdOutputStyle(args);
    } else if (UserCmds::exists(cmd)) {
        // A user's own command from .ollamadev/commands/<name>.md. It expands to a
        // prompt template, so it is a turn, not a builtin — without this the REPL
        // says "unknown command" for a command the CLI itself happily runs.
        s.prompt = UserCmds::expand(cmd, args);
    } else {
        s.handled = false;
    }

    if (!out.isEmpty()) emitRaw(out);
    return s;
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

int Repl::run() {
    Tools::registerAll();
    Tools::setThreadRoot(QDir::currentPath());

    const bool interactive = Tui::stdinIsTty() && Tui::stdoutIsTty();
    Permission::setInteractive(interactive);
    Permission::setMode(Permission::modeFromName(
        Config::str(QStringLiteral("permissions.mode"), QStringLiteral("ask"))));

    Permission::setAsker([this](const ToolDef& t, const QJsonObject& args) {
        ToolCall c;
        c.name = t.name;
        c.args = args;
        emitRaw(QLatin1Char('\n') + yellow(QStringLiteral("  ⚠ ")) + t.name +
                QLatin1Char(' ') + dim(describeArgs(c)) + QLatin1Char('\n'));
        emitRaw(dim(QStringLiteral("  allow? [y/N/a=allow everything this session]")));
        const auto line = readRawLine();
        if (!line) return false;
        const QString a = line->trimmed().toLower();
        if (a == QLatin1String("a")) {
            Permission::setMode(PermMode::Auto);
            return true;
        }
        return a == QLatin1String("y") || a == QLatin1String("yes");
    });

    Permission::setPlanApprover([this](const QString& plan) {
        emitRaw(QLatin1Char('\n') + cyan(QStringLiteral("  📋 proposed plan")) + QLatin1Char('\n') +
                (Render::enabled() ? Render::markdown(plan) : plan) + QLatin1Char('\n'));
        if (!confirm(dim(QStringLiteral("  approve and start implementing? [y/N]")))) return false;
        rebuildPrompts();
        return true;
    });

    // SIGINT stops the RESPONSE, not the process. In the line editor Ctrl-C never
    // even reaches here (raw mode delivers it as a byte), so this only fires while
    // a turn is streaming — exactly where we want it.
    std::signal(SIGINT, onSigint);

    banner();
    preflight();
    if (opt_.resume || !opt_.sessionId.isEmpty()) resumeNotice();

    // A prompt given on the command line runs before we block on input, so
    // `ollamadev chat "hi"` answers immediately and then keeps the thread open.
    if (!opt_.initialPrompt.isEmpty()) runTurn(opt_.initialPrompt);

    while (true) {
        const auto raw = readInput();
        if (!raw) {  // EOF / Ctrl-D
            emitRaw(QStringLiteral("\n"));
            break;
        }
        const QString input = raw->trimmed();
        if (input.isEmpty()) continue;

        history_ << input;

        if (input.startsWith(QLatin1Char('/'))) {
            const Slash s = slash(input);
            if (s.quit) break;
            if (!s.prompt.isEmpty()) {
                runTurn(s.prompt);  // a user command expands to a prompt, so it is a turn
                continue;
            }
            if (!s.handled)
                emitRaw(yellow(QStringLiteral("  unknown command: ")) + input + QLatin1Char('\n') +
                        dim(QStringLiteral("  /help lists them all\n")));
            continue;
        }
        if (input == QLatin1String("exit") || input == QLatin1String("quit")) {
            session_.save();
            emitRaw(dim(QStringLiteral("  bye\n")));
            break;
        }

        runTurn(input);
    }

    session_.save();
    std::signal(SIGINT, SIG_DFL);
    return 0;
}

}  // namespace odv
