#include "Lsp.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>

#include "Backend.h"
#include "Config.h"
#include "Models.h"
#include "Parallel.h"
#include "Tools.h"
#include "Version.h"

#ifdef Q_OS_WIN
#include <io.h>
#define ODV_DUP _dup
#define ODV_DUP2 _dup2
#define ODV_READ _read
#define ODV_WRITE _write
#define ODV_CLOSE _close
#define ODV_FD_IN 0
#define ODV_FD_OUT 1
#define ODV_FD_ERR 2
#else
#include <unistd.h>
#define ODV_DUP ::dup
#define ODV_DUP2 ::dup2
#define ODV_READ ::read
#define ODV_WRITE ::write
#define ODV_CLOSE ::close
#define ODV_FD_IN STDIN_FILENO
#define ODV_FD_OUT STDOUT_FILENO
#define ODV_FD_ERR STDERR_FILENO
#endif

namespace odv {
namespace {

// --------------------------------------------------------------- stderr logging

// The ONLY output call in this file that is not a protocol frame. It writes to fd
// 2 directly rather than through QTextStream(stdout) — the whole class of bug this
// server has to avoid is "something printed to stdout".
void logMsg(const QString& s) {
    const QByteArray b = (QStringLiteral("[lsp] ") + s + QLatin1Char('\n')).toUtf8();
    qint64 off = 0;
    while (off < b.size()) {
        const auto n = ODV_WRITE(ODV_FD_ERR, b.constData() + off,
                                 static_cast<unsigned>(b.size() - off));
        if (n <= 0) return;
        off += n;
    }
}

// ------------------------------------------------------------------ URI <-> path

QString uriToPath(const QString& uri) {
    if (uri.startsWith(QLatin1String("file:"))) return QUrl(uri).toLocalFile();
    return uri;
}

QString pathToUri(const QString& path) {
    return QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString();
}

// ------------------------------------------------------------------ framing

// Extract ONE Content-Length-framed message from `rx`, consuming its bytes.
// Returns false when the buffer does not yet hold a complete message.
//
// The length is a BYTE count, so the body is cut from the QByteArray before any
// UTF-8 decoding — measuring a decoded QString would mis-frame the moment a
// document contains a non-ASCII character, and everything after it would be junk.
bool takeFrame(QByteArray* rx, QJsonObject* out) {
    // The spec says \r\n\r\n. Some clients emit \n\n; accept both on the way in,
    // always emit \r\n\r\n on the way out.
    int sepLen = 4;
    int sep = rx->indexOf("\r\n\r\n");
    const int lf = rx->indexOf("\n\n");
    if (sep < 0 || (lf >= 0 && lf < sep)) {
        if (lf < 0) return false;
        sep = lf;
        sepLen = 2;
    }
    if (sep < 0) return false;

    static const QRegularExpression lenRe(QStringLiteral("Content-Length:\\s*(\\d+)"),
                                          QRegularExpression::CaseInsensitiveOption);
    const auto m = lenRe.match(QString::fromLatin1(rx->left(sep)));
    if (!m.hasMatch()) {
        // Unframed junk: drop the header block rather than spin on it forever.
        rx->remove(0, sep + sepLen);
        logMsg(QStringLiteral("dropped a header block with no Content-Length"));
        return false;
    }

    const int want = m.captured(1).toInt();
    if (rx->size() < sep + sepLen + want) return false;  // body still in flight

    const QByteArray body = rx->mid(sep + sepLen, want);
    rx->remove(0, sep + sepLen + want);

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        logMsg(QStringLiteral("malformed JSON-RPC body: %1").arg(err.errorString()));
        *out = QJsonObject();
        return true;  // consumed; the caller just gets an empty message
    }
    *out = doc.object();
    return true;
}

QByteArray frame(const QJsonObject& msg) {
    const QByteArray body = QJsonDocument(msg).toJson(QJsonDocument::Compact);
    return "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
}

// ------------------------------------------------------------------ transports

class Transport {
public:
    virtual ~Transport() = default;
    // Blocks until one whole message arrives. False = the peer is gone.
    virtual bool read(QJsonObject* msg) = 0;
    virtual void write(const QJsonObject& msg) = 0;
};

// stdin/stdout, with fd 1 confiscated for the protocol.
//
// PHP's ob_start() could not have done this job: an output buffer catches what
// the interpreter prints, not what a child process writes to the descriptor it
// inherited. Moving the descriptor itself catches everything.
class StdioTransport final : public Transport {
public:
    StdioTransport() {
        channel_ = ODV_DUP(ODV_FD_OUT);
        if (channel_ < 0) return;
        // Deliberately NOT fflush(stdout) first: anything already buffered would be
        // flushed onto the real fd 1 — corrupting the channel we are protecting.
        // Left buffered, it drains to stderr at the dup2 below instead.
        ODV_DUP2(ODV_FD_ERR, ODV_FD_OUT);
    }
    ~StdioTransport() override {
        if (channel_ < 0) return;
        fflush(stdout);  // drains to fd 1, which is stderr right now
        ODV_DUP2(channel_, ODV_FD_OUT);
        ODV_CLOSE(channel_);
    }

    bool ok() const { return channel_ >= 0; }

    bool read(QJsonObject* msg) override {
        for (;;) {
            if (takeFrame(&rx_, msg)) return true;
            char buf[8192];
            const auto n = ODV_READ(ODV_FD_IN, buf, sizeof(buf));
            if (n <= 0) return false;  // EOF: the editor closed the pipe
            rx_.append(buf, static_cast<int>(n));
        }
    }

    void write(const QJsonObject& msg) override {
        const QByteArray b = frame(msg);
        qint64 off = 0;
        while (off < b.size()) {
            const auto n = ODV_WRITE(channel_, b.constData() + off,
                                     static_cast<unsigned>(b.size() - off));
            if (n <= 0) return;  // client hung up; the read loop will see EOF too
            off += n;
        }
    }

private:
    int channel_ = -1;  // the REAL stdout; nothing else may write here
    QByteArray rx_;
};

class SocketTransport final : public Transport {
public:
    explicit SocketTransport(QTcpSocket* s) : sock_(s) {}

    bool read(QJsonObject* msg) override {
        for (;;) {
            if (takeFrame(&rx_, msg)) return true;
            if (sock_->state() != QAbstractSocket::ConnectedState && sock_->bytesAvailable() == 0)
                return false;
            if (!sock_->waitForReadyRead(-1)) return false;
            rx_.append(sock_->readAll());
        }
    }

    void write(const QJsonObject& msg) override {
        sock_->write(frame(msg));
        sock_->waitForBytesWritten(5000);
    }

private:
    QTcpSocket* sock_;
    QByteArray rx_;
};

// ------------------------------------------------------------------ text helpers

QStringList splitLines(const QString& text) {
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString& l : lines)
        if (l.endsWith(QLatin1Char('\r'))) l.chop(1);
    return lines;
}

// LSP positions count UTF-16 code units, which is exactly what a QString index is
// — so no conversion is needed here, and none should be added.
//
// NOTE the +1 per line: splitLines() dropped the '\n' that separated them, and the
// offset is into the ORIGINAL text. Forgetting it drifts the cursor one character
// further left with every line of the file.
int offsetOf(const QStringList& lines, int line, int character) {
    int off = 0;
    const int last = qMin(line, lines.size());
    for (int i = 0; i < last; ++i) off += lines.at(i).size() + 1;
    if (line >= 0 && line < lines.size())
        off += qBound(0, character, lines.at(line).size());
    return off;
}

bool isWordChar(QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); }

// The identifier under the cursor, plus its [start,end) columns on that line.
QString wordAt(const QStringList& lines, int line, int character, int* start = nullptr,
               int* end = nullptr) {
    if (line < 0 || line >= lines.size()) return {};
    const QString& l = lines.at(line);
    int i = qBound(0, character, l.size());
    // A cursor sitting just past the end of a word still means that word.
    if (i > 0 && (i == l.size() || !isWordChar(l.at(i)))) --i;
    if (i >= l.size() || !isWordChar(l.at(i))) return {};
    int a = i;
    while (a > 0 && isWordChar(l.at(a - 1))) --a;
    int b = i;
    while (b + 1 < l.size() && isWordChar(l.at(b + 1))) ++b;
    if (start) *start = a;
    if (end) *end = b + 1;
    return l.mid(a, b + 1 - a);
}

QJsonObject position(int line, int character) {
    return QJsonObject{{"line", line}, {"character", character}};
}

QJsonObject range(int l1, int c1, int l2, int c2) {
    return QJsonObject{{"start", position(l1, c1)}, {"end", position(l2, c2)}};
}

// Declaration-ish lines, across the languages this thing is likely to see. Used
// for both hover and go-to-definition; the PHP shelled out to grep for this, which
// meant a word with a regex metacharacter in it silently searched for something
// else. Escaped and matched in-process instead.
QRegularExpression definitionRe(const QString& word) {
    const QString w = QRegularExpression::escape(word);
    return QRegularExpression(
        QStringLiteral("(?:^|\\s)(?:function|class|def|fn|struct|interface|enum|type|trait|impl|"
                       "const|let|var|public|private|protected|static)\\s+%1\\b|"
                       "\\b%1\\s*(?:=\\s*(?:function|\\()|:=|\\()")
            .arg(w));
}

// ------------------------------------------------------------------ limiter key

// Same STRINGS as core/Crew.cpp::limiterKey — the semaphore is keyed by string, so
// matching the crew's keys is what stops an editor's completions from opening a
// second pool of local GPU slots on top of a running crew's.
QString limiterKey(const QString& backendId, const QString& model) {
    if (backendId == QLatin1String("ollama")) {
        return Models::isCloud(model) ? QStringLiteral("ollama:cloud")
                                      : QStringLiteral("ollama:local");
    }
    return backendId;
}

// ------------------------------------------------------------------ linter plumbing

struct LintOut {
    int exitCode = -1;
    QString text;  // stdout + stderr, merged: gcc talks on stderr, go vet on stdout
};

// program + argv array, an explicit working directory, and a hard timeout. Never a
// shell string: a path with a space or a quote in it would otherwise become two
// arguments, and a linter is handed user-controlled paths by definition.
LintOut runLinter(const QString& program, const QStringList& args, const QString& workDir) {
    LintOut r;
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.setWorkingDirectory(workDir);
    p.start(program, args);
    if (!p.waitForStarted(5000)) {
        r.text = QStringLiteral("could not start %1").arg(program);
        return r;
    }
    const int timeoutMs = qMax(5, Config::integer("lsp.lintTimeout", 20)) * 1000;
    if (!p.waitForFinished(timeoutMs)) {
        // Our OWN child, terminated by handle. A kill by name would take out an
        // identical compiler that some other process on this box is running.
        p.terminate();
        if (!p.waitForFinished(2000)) p.kill();
        r.text = QStringLiteral("%1 timed out").arg(program);
        return r;
    }
    r.text = QString::fromUtf8(p.readAll());
    r.exitCode = p.exitCode();
    return r;
}

QString which(const QString& bin) { return QStandardPaths::findExecutable(bin); }

// python3 first: on most Linux distributions a bare `python` does not exist.
QString pythonBin() {
    const QString p3 = which(QStringLiteral("python3"));
    return p3.isEmpty() ? which(QStringLiteral("python")) : p3;
}

Diagnostic diag(int line1, int col1, const QString& msg, const QString& source, int severity = 1) {
    Diagnostic d;
    d.line = qMax(0, line1 - 1);  // linters count from 1, LSP from 0
    d.col = qMax(0, col1 - 1);
    d.endLine = d.line;
    d.endCol = d.col + 1;
    d.severity = severity;
    d.message = msg.trimmed();
    d.source = source;
    return d;
}

// file:line:col: [severity:] message — what gcc, g++ and
// `rustc --error-format=short` all emit.
//
// The severity token is OPTIONAL because `go vet` does not print one: its lines
// are a bare `./main.go:5:15: fmt.Printf format %d has arg …`. Requiring
// "error:"/"warning:" here silently matched nothing for Go and the editor got an
// empty diagnostic list for a file vet had plenty to say about. An unlabelled
// finding is an error — vet only speaks up when something is wrong.
QVector<Diagnostic> parseGnuStyle(const QString& out, const QString& source) {
    QVector<Diagnostic> diags;
    static const QRegularExpression re(QStringLiteral(
        "^(.+?):(\\d+):(\\d+):\\s*(?:(error|warning|note)[^:]*:\\s*)?(.+)$"));
    for (const QString& line : out.split(QLatin1Char('\n'))) {
        const auto m = re.match(line);
        if (!m.hasMatch()) continue;
        const QString sev = m.captured(4);
        diags << diag(m.captured(2).toInt(), m.captured(3).toInt(), m.captured(5), source,
                      sev == QLatin1String("warning") ? 2
                                                      : (sev == QLatin1String("note") ? 3 : 1));
    }
    return diags;
}

}  // namespace

// ================================================================= Linters

QString Linters::toolFor(const QString& path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QLatin1String("php")) return which(QStringLiteral("php")).isEmpty() ? QString() : QStringLiteral("php -l");
    if (ext == QLatin1String("py")) return pythonBin().isEmpty() ? QString() : QStringLiteral("py_compile");
    if (ext == QLatin1String("go")) return which(QStringLiteral("go")).isEmpty() ? QString() : QStringLiteral("go vet");
    if (ext == QLatin1String("c") || ext == QLatin1String("h"))
        return which(QStringLiteral("gcc")).isEmpty() ? QString() : QStringLiteral("gcc");
    if (ext == QLatin1String("cpp") || ext == QLatin1String("cc") || ext == QLatin1String("cxx") ||
        ext == QLatin1String("hpp"))
        return which(QStringLiteral("g++")).isEmpty() ? QString() : QStringLiteral("g++");
    if (ext == QLatin1String("rs")) return which(QStringLiteral("rustc")).isEmpty() ? QString() : QStringLiteral("rustc");
    return {};
}

QVector<Diagnostic> Linters::run(const QString& path, const QString& text) {
    const QFileInfo fi(path);
    const QString ext = fi.suffix().toLower();
    const QString base = fi.fileName();
    if (base.isEmpty()) return {};

    // go vet analyses a PACKAGE, not a file: it needs the module context that only
    // the real directory has (a lone temp copy has no go.mod and vet refuses). So Go
    // is the one language linted from the file's own directory, against what is
    // SAVED there. `go vet` is an analyser — it reads, it does not write — and the
    // working directory is passed explicitly, never inherited from the process cwd.
    if (ext == QLatin1String("go")) {
        const QString go = which(QStringLiteral("go"));
        if (go.isEmpty()) return {};
        QString dir = fi.absolutePath();
        if (dir.isEmpty() || !QDir(dir).exists())
            dir = Tools::hasThreadRoot() ? Tools::threadRoot() : QDir::currentPath();
        // `go vet [build flags] [vet flags] [packages]` — verified with `go help vet`.
        // It reports ./file.go:line:col: msg, already scoped to this package.
        const LintOut r = runLinter(go, {QStringLiteral("vet"), QStringLiteral(".")}, dir);
        return parseGnuStyle(r.text, QStringLiteral("go vet"));
    }

    // Everything else lints the BUFFER, so the squiggles track unsaved edits. The
    // copy also keeps each tool's droppings — __pycache__, .rmeta, object files —
    // inside a temp dir that dies with this call, instead of in the user's repo.
    QTemporaryDir tmp;
    if (!tmp.isValid()) return {};
    const QString file = tmp.path() + QLatin1Char('/') + base;
    {
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
        f.write(text.toUtf8());
    }

    if (ext == QLatin1String("php")) {
        const QString php = which(QStringLiteral("php"));
        if (php.isEmpty()) return {};
        // `-l  Syntax check only (lint)` — verified with `php --help`.
        const LintOut r = runLinter(php, {QStringLiteral("-l"), file}, tmp.path());
        if (r.exitCode == 0) return {};
        // "PHP Parse error:  syntax error, unexpected … in <file> on line N"
        static const QRegularExpression re(
            QStringLiteral("^(?:PHP )?(?:Parse|Fatal) error:\\s*(.*) in .* on line (\\d+)"));
        QVector<Diagnostic> diags;
        for (const QString& line : r.text.split(QLatin1Char('\n'))) {
            const auto m = re.match(line.trimmed());
            if (m.hasMatch())
                diags << diag(m.captured(2).toInt(), 1, m.captured(1), QStringLiteral("php -l"));
        }
        return diags;
    }

    if (ext == QLatin1String("py")) {
        const QString py = pythonBin();
        if (py.isEmpty()) return {};
        // `py_compile.py [-h] [-q] filenames …` — verified with `python3 -m py_compile --help`.
        const LintOut r = runLinter(py, {QStringLiteral("-m"), QStringLiteral("py_compile"), file},
                                    tmp.path());
        if (r.exitCode == 0) return {};

        // The traceback is three lines:  File "…", line N / the source / a caret / SyntaxError: msg
        static const QRegularExpression at(QStringLiteral("File \"[^\"]*\", line (\\d+)"));
        static const QRegularExpression err(QStringLiteral("^(\\w*(?:Error|Warning)):\\s*(.*)$"));
        int line = 0;
        int col = 1;
        QString message;
        const QStringList lines = r.text.split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            const auto ma = at.match(lines.at(i));
            if (ma.hasMatch()) {
                line = ma.captured(1).toInt();
                // The caret line points at the column; python indents both by 4.
                for (int j = i + 1; j < lines.size() && j <= i + 3; ++j) {
                    const int caret = lines.at(j).indexOf(QLatin1Char('^'));
                    if (caret >= 0) {
                        col = qMax(1, caret - 3);
                        break;
                    }
                }
            }
            const auto me = err.match(lines.at(i).trimmed());
            if (me.hasMatch()) message = me.captured(1) + QStringLiteral(": ") + me.captured(2);
        }
        if (line <= 0) return {};
        if (message.isEmpty()) message = QStringLiteral("compile error");
        return {diag(line, col, message, QStringLiteral("py_compile"))};
    }

    if (ext == QLatin1String("c") || ext == QLatin1String("h") || ext == QLatin1String("cpp") ||
        ext == QLatin1String("cc") || ext == QLatin1String("cxx") || ext == QLatin1String("hpp")) {
        const bool cpp = (ext != QLatin1String("c") && ext != QLatin1String("h"));
        const QString cc = which(cpp ? QStringLiteral("g++") : QStringLiteral("gcc"));
        if (cc.isEmpty()) return {};
        // `-fsyntax-only  Check for syntax errors, then stop.` and `-I <dir>  Add
        // <dir> to the end of the main include path.` — both verified with
        // `gcc --help=common` / `gcc -v --help`. -I points at the file's REAL folder
        // so its own #include "…" headers still resolve from the temp copy.
        QStringList args{QStringLiteral("-fsyntax-only")};
        if (fi.absolutePath().length() > 1) args << QStringLiteral("-I") << fi.absolutePath();
        args << file;
        const LintOut r = runLinter(cc, args, tmp.path());
        if (r.exitCode == 0) return {};
        return parseGnuStyle(r.text, cpp ? QStringLiteral("g++") : QStringLiteral("gcc"));
    }

    if (ext == QLatin1String("rs")) {
        const QString rustc = which(QStringLiteral("rustc"));
        if (rustc.isEmpty()) return {};
        // --emit=metadata: type-check without linking, which is all we want and the
        // only mode that works on a file that is not a whole crate. --out-dir keeps
        // the artefact in the temp dir. --error-format=short turns rustc's beautiful
        // multi-line diagnostics into the same file:line:col: form gcc uses.
        // Verified with `rustc --help`.
        const LintOut r = runLinter(rustc,
                                    {QStringLiteral("--edition"), QStringLiteral("2021"),
                                     QStringLiteral("--crate-type"), QStringLiteral("lib"),
                                     QStringLiteral("--emit=metadata"),
                                     QStringLiteral("--error-format=short"),
                                     QStringLiteral("--out-dir"), tmp.path(), file},
                                    tmp.path());
        if (r.exitCode == 0) return {};
        return parseGnuStyle(r.text, QStringLiteral("rustc"));
    }

    return {};
}

// ================================================================= the server

namespace {

struct Doc {
    QString text;
    int version = 0;
};

class Server {
public:
    Server(Transport* t, LspOptions o) : t_(t), o_(std::move(o)) {}

    // Runs until the client closes the connection or sends `exit`.
    int run() {
        QJsonObject msg;
        while (t_->read(&msg)) {
            if (msg.isEmpty()) continue;
            handle(msg);
            if (exit_) break;
        }
        // The spec: exit after a shutdown request is a clean 0; a client that just
        // vanishes, or exits without shutting down, is a 1.
        return shutdownRequested_ ? 0 : 1;
    }

private:
    // ---- wire ------------------------------------------------------------
    void reply(const QJsonValue& id, const QJsonValue& result) {
        t_->write(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
    }
    void replyError(const QJsonValue& id, int code, const QString& message) {
        t_->write(QJsonObject{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"error", QJsonObject{{"code", code}, {"message", message}}}});
    }
    void notify(const QString& method, const QJsonObject& params) {
        t_->write(QJsonObject{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
    }

    // ---- documents -------------------------------------------------------
    void publishDiagnostics(const QString& uri) {
        const QString path = uriToPath(uri);
        QJsonArray items;
        for (const Diagnostic& d : Linters::run(path, docs_.value(uri).text)) {
            items.append(QJsonObject{
                {"range", range(d.line, d.col, d.endLine, d.endCol)},
                {"severity", d.severity},
                {"source", d.source},
                {"message", d.message},
            });
        }
        // Published even when empty: that is how the editor learns the squiggles from
        // the last edit are gone. Skipping the empty case is the reason stale errors
        // stick to a file the user already fixed.
        notify(QStringLiteral("textDocument/publishDiagnostics"),
               QJsonObject{{"uri", uri}, {"diagnostics", items}});
    }

    // ---- features --------------------------------------------------------

    QJsonValue completion(const QJsonObject& params) {
        const QJsonObject td = params.value(QStringLiteral("textDocument")).toObject();
        const QString uri = td.value(QStringLiteral("uri")).toString();
        if (!docs_.contains(uri)) return QJsonArray();

        const QJsonObject pos = params.value(QStringLiteral("position")).toObject();
        const int line = pos.value(QStringLiteral("line")).toInt();
        const int chr = pos.value(QStringLiteral("character")).toInt();

        const QString text = docs_.value(uri).text;
        const QStringList lines = splitLines(text);
        const int off = offsetOf(lines, line, chr);

        // A bounded window, not the whole file: a completion that takes ten seconds
        // has already been abandoned by the editor.
        const int back = qMax(0, off - 4000);
        const QString before = text.mid(back, off - back);
        const QString after = text.mid(off, 1500);

        auto b = Backends::get(backendId());
        if (!b || !b->available()) return QJsonArray();

        QVector<ChatMessage> msgs;
        msgs.push_back({"system",
                        "You are a code completion engine inside an editor. Continue the code at "
                        "the cursor marker. Reply with ONLY the raw code to insert at the cursor: "
                        "no explanation, no commentary, no markdown fences. If nothing sensible "
                        "can follow, reply with nothing at all.",
                        {}, {}, {}, {}, {}});
        msgs.push_back({"user",
                        QStringLiteral("File: %1\n\n%2<CURSOR>%3\n\nInsert at <CURSOR>.")
                            .arg(uriToPath(uri), before, after),
                        {}, {}, {}, {}, {}});

        // Through the crew's admission control, on the crew's key: an editor firing a
        // completion on every keystroke must queue behind a running crew for the local
        // GPU rather than fight it for the same slots.
        StreamSink sink;
        CancelToken cancel;
        ChatTurn turn;
        {
            auto permit = Limiter::instance().acquire(limiterKey(backendId(), model()));
            turn = b->chat(model(), msgs, QJsonArray(), sink, cancel);
        }
        if (!turn.ok || turn.content.trimmed().isEmpty()) return QJsonArray();

        QString insert = turn.content;
        // Models fence code even when told not to — the same habit that makes
        // json::decodeLoose necessary elsewhere.
        static const QRegularExpression fence(QStringLiteral("^\\s*```[a-zA-Z0-9_+-]*\\n?|```\\s*$"));
        insert.remove(fence);
        insert = insert.trimmed();
        if (insert.isEmpty()) return QJsonArray();

        const QString label = insert.split(QLatin1Char('\n')).first().trimmed();
        QJsonArray items;
        items.append(QJsonObject{
            {"label", label.isEmpty() ? QStringLiteral("AI completion") : label.left(60)},
            {"kind", 1},  // Text: this is a snippet of code, not a known symbol
            {"detail", QStringLiteral("OllamaDev · %1").arg(model())},
            {"insertText", insert},
            {"insertTextFormat", 1},  // plain text, NOT a snippet: no $ or {} expansion
            {"documentation", insert},
        });
        return items;
    }

    QJsonValue hover(const QJsonObject& params) {
        const QString uri = params.value(QStringLiteral("textDocument"))
                                .toObject()
                                .value(QStringLiteral("uri"))
                                .toString();
        if (!docs_.contains(uri)) return QJsonValue(QJsonValue::Null);
        const QJsonObject pos = params.value(QStringLiteral("position")).toObject();
        const int line = pos.value(QStringLiteral("line")).toInt();
        const int chr = pos.value(QStringLiteral("character")).toInt();

        const QStringList lines = splitLines(docs_.value(uri).text);
        int a = 0;
        int b = 0;
        const QString word = wordAt(lines, line, chr, &a, &b);
        if (word.isEmpty()) return QJsonValue(QJsonValue::Null);

        // Where is it declared? Same search as go-to-definition, but rendered.
        QStringList hits;
        const QRegularExpression re = definitionRe(word);
        for (int i = 0; i < lines.size() && hits.size() < 5; ++i) {
            if (re.match(lines.at(i)).hasMatch())
                hits << QStringLiteral("%1: %2").arg(i + 1).arg(lines.at(i).trimmed());
        }

        QString md = QStringLiteral("**%1**\n\n").arg(word);
        if (hits.isEmpty()) {
            md += QStringLiteral("```\n%1\n```\n").arg(lines.value(line).trimmed());
            md += QStringLiteral("\n_no declaration found in this file_");
        } else {
            md += QStringLiteral("```\n%1\n```").arg(hits.join(QLatin1Char('\n')));
        }

        return QJsonObject{
            {"contents", QJsonObject{{"kind", "markdown"}, {"value", md}}},
            {"range", range(line, a, line, b)},
        };
    }

    QJsonValue definition(const QJsonObject& params) {
        const QString uri = params.value(QStringLiteral("textDocument"))
                                .toObject()
                                .value(QStringLiteral("uri"))
                                .toString();
        if (!docs_.contains(uri)) return QJsonValue(QJsonValue::Null);
        const QJsonObject pos = params.value(QStringLiteral("position")).toObject();
        const int line = pos.value(QStringLiteral("line")).toInt();
        const int chr = pos.value(QStringLiteral("character")).toInt();

        const QStringList lines = splitLines(docs_.value(uri).text);
        const QString word = wordAt(lines, line, chr);
        if (word.isEmpty()) return QJsonValue(QJsonValue::Null);
        const QRegularExpression re = definitionRe(word);

        // The open buffer first — it is the only text we know is current. A hit on
        // the cursor's OWN line is kept as a fallback rather than skipped: when the
        // cursor is already on the declaration, "here" is the right answer, and a
        // hard skip would make go-to-definition silently fail on every one-line
        // definition in the file.
        int selfLine = -1;
        for (int i = 0; i < lines.size(); ++i) {
            if (!re.match(lines.at(i)).hasMatch()) continue;
            if (i == line) {
                selfLine = i;
                continue;
            }
            const int col = qMax(0, lines.at(i).indexOf(word));
            return QJsonObject{{"uri", uri}, {"range", range(i, col, i, col + word.size())}};
        }
        if (selfLine >= 0) {
            const int col = qMax(0, lines.at(selfLine).indexOf(word));
            return QJsonObject{
                {"uri", uri}, {"range", range(selfLine, col, selfLine, col + word.size())}};
        }

        // Then its siblings on disk. Bounded to the one directory and to files of the
        // same kind: an LSP request must answer in milliseconds, so this is a
        // neighbourhood search, not a project-wide index (that is what `index build`
        // and the code-search tool are for).
        const QFileInfo fi(uriToPath(uri));
        QDir dir(fi.absolutePath());
        const QStringList sameKind = fi.suffix().isEmpty()
                                         ? QStringList{QStringLiteral("*")}
                                         : QStringList{QStringLiteral("*.") + fi.suffix()};
        int scanned = 0;
        for (const QFileInfo& sib : dir.entryInfoList(sameKind, QDir::Files, QDir::Name)) {
            if (++scanned > 200) break;
            if (sib.absoluteFilePath() == fi.absoluteFilePath()) continue;
            QFile f(sib.absoluteFilePath());
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QStringList sl = splitLines(QString::fromUtf8(f.read(512 * 1024)));
            for (int i = 0; i < sl.size(); ++i) {
                if (!re.match(sl.at(i)).hasMatch()) continue;
                const int col = qMax(0, sl.at(i).indexOf(word));
                return QJsonObject{{"uri", pathToUri(sib.absoluteFilePath())},
                                   {"range", range(i, col, i, col + word.size())}};
            }
        }
        return QJsonValue(QJsonValue::Null);  // null: "I looked, there is nothing"
    }

    // ---- symbols, references, rename -------------------------------------

    // A document's top-level symbols, by regex. Deliberately NOT a parser: an LSP
    // request has to answer in milliseconds, across every language the editor might
    // open, and a wrong-but-instant outline beats a correct one that arrives after
    // the user has scrolled away. Anything better than this belongs in `index build`.
    QJsonArray documentSymbol(const QJsonObject& params) {
        const QString uri = params.value(QStringLiteral("textDocument"))
                                .toObject()
                                .value(QStringLiteral("uri"))
                                .toString();
        QJsonArray out;
        if (!docs_.contains(uri)) return out;
        const QStringList lines = splitLines(docs_.value(uri).text);

        // name, kind (LSP SymbolKind), pattern
        static const struct {
            int kind;
            const char* re;
        } kPatterns[] = {
            {5, R"(^\s*(?:public\s+|final\s+|abstract\s+)*class\s+(\w+))"},          // Class
            {23, R"(^\s*struct\s+(\w+))"},                                            // Struct
            {10, R"(^\s*enum(?:\s+class)?\s+(\w+))"},                                 // Enum
            {11, R"(^\s*(?:interface|trait|protocol)\s+(\w+))"},                      // Interface
            {12, R"(^\s*(?:def|fn|func|function)\s+(\w+))"},                          // Function
            {12, R"(^\s*(?:[\w:<>,\*&\s]+\s+)?(\w+)\s*\([^;]*\)\s*(?:const\s*)?\{)"},  // C-ish fn
            {13, R"(^\s*(?:const|let|var)\s+(\w+))"},                                 // Variable
        };

        for (int i = 0; i < lines.size(); ++i) {
            const QString& l = lines.at(i);
            for (const auto& p : kPatterns) {
                static QHash<QString, QRegularExpression> cache;
                const QString key = QString::fromLatin1(p.re);
                if (!cache.contains(key)) cache.insert(key, QRegularExpression(key));
                const auto m = cache.value(key).match(l);
                if (!m.hasMatch()) continue;
                const QString name = m.captured(1);
                if (name.isEmpty()) continue;
                const int col = qMax(0, l.indexOf(name));
                out.append(QJsonObject{
                    {"name", name},
                    {"kind", p.kind},
                    {"range", range(i, 0, i, l.size())},
                    {"selectionRange", range(i, col, i, col + name.size())},
                });
                break;  // one symbol per line; the first pattern that matches wins
            }
        }
        return out;
    }

    // Every occurrence of the symbol under the cursor, in the OPEN BUFFER only.
    // Deliberately not project-wide: the buffer is the only text we know is current,
    // and an editor that gets a stale location has sent the user somewhere wrong.
    QJsonArray references(const QJsonObject& params) {
        const QString uri = params.value(QStringLiteral("textDocument"))
                                .toObject()
                                .value(QStringLiteral("uri"))
                                .toString();
        QJsonArray out;
        if (!docs_.contains(uri)) return out;
        const QJsonObject pos = params.value(QStringLiteral("position")).toObject();
        const QStringList lines = splitLines(docs_.value(uri).text);
        const QString word = wordAt(lines, pos.value(QStringLiteral("line")).toInt(),
                                    pos.value(QStringLiteral("character")).toInt());
        if (word.isEmpty()) return out;

        // \b…\b: renaming `id` must not match the `id` inside `width`.
        const QRegularExpression re(QStringLiteral("\\b") + QRegularExpression::escape(word) +
                                    QStringLiteral("\\b"));
        for (int i = 0; i < lines.size(); ++i) {
            auto it = re.globalMatch(lines.at(i));
            while (it.hasNext()) {
                const auto m = it.next();
                out.append(QJsonObject{
                    {"uri", uri},
                    {"range", range(i, m.capturedStart(), i, m.capturedEnd())}});
            }
        }
        return out;
    }

    QJsonValue rename(const QJsonObject& params) {
        const QString uri = params.value(QStringLiteral("textDocument"))
                                .toObject()
                                .value(QStringLiteral("uri"))
                                .toString();
        const QString newName = params.value(QStringLiteral("newName")).toString();
        if (!docs_.contains(uri) || newName.isEmpty()) return QJsonValue(QJsonValue::Null);

        // Reuse references(): the set of edits a rename makes IS the set of
        // references it found. Two implementations of "where is this symbol" would
        // eventually disagree, and the day they do, a rename corrupts the file.
        const QJsonArray refs = references(params);
        if (refs.isEmpty()) return QJsonValue(QJsonValue::Null);

        QJsonArray edits;
        for (const QJsonValue& r : refs)
            edits.append(QJsonObject{{"range", r.toObject().value(QStringLiteral("range"))},
                                     {"newText", newName}});
        return QJsonObject{{"changes", QJsonObject{{uri, edits}}}};
    }

    // ---- formatting ------------------------------------------------------

    // The formatter for a file, or empty if we have none. Config wins; otherwise a
    // small table of tools, each used ONLY if it is actually installed, and each
    // invoked the way its own --help says to (gofmt reads stdin when given no path;
    // rustfmt needs --emit stdout). We do not guess, and we do not pretend: with no
    // formatter available, formatting returns no edits rather than mangling the file.
    QStringList formatterFor(const QString& path) const {
        const QString ext = QFileInfo(path).suffix().toLower();
        const QString configured = Config::str(QStringLiteral("format.") + ext, QString());
        if (!configured.isEmpty()) return configured.split(QLatin1Char(' '), Qt::SkipEmptyParts);

        struct Fmt {
            const char* ext;
            QStringList argv;
        };
        const QVector<Fmt> table{
            {"go", {QStringLiteral("gofmt")}},
            {"rs", {QStringLiteral("rustfmt"), QStringLiteral("--emit"), QStringLiteral("stdout")}},
            {"py", {QStringLiteral("black"), QStringLiteral("-q"), QStringLiteral("-")}},
            {"c", {QStringLiteral("clang-format")}},
            {"h", {QStringLiteral("clang-format")}},
            {"cc", {QStringLiteral("clang-format")}},
            {"cpp", {QStringLiteral("clang-format")}},
            {"hpp", {QStringLiteral("clang-format")}},
        };
        for (const Fmt& f : table) {
            if (ext != QLatin1String(f.ext)) continue;
            if (QStandardPaths::findExecutable(f.argv.first()).isEmpty()) return {};
            return f.argv;
        }
        return {};
    }

    QJsonArray formatting(const QJsonObject& params) {
        const QString uri = params.value(QStringLiteral("textDocument"))
                                .toObject()
                                .value(QStringLiteral("uri"))
                                .toString();
        QJsonArray out;
        if (!docs_.contains(uri)) return out;

        const QString path = uriToPath(uri);
        const QStringList argv = formatterFor(path);
        if (argv.isEmpty()) return out;  // no formatter: no edits, not a mangled file

        const QString text = docs_.value(uri).text;
        QProcess p;
        p.setProgram(argv.first());
        p.setArguments(argv.mid(1));  // argv array: a path with a space is not two args
        p.setWorkingDirectory(rootPath_.isEmpty() ? QFileInfo(path).absolutePath() : rootPath_);
        p.start();
        if (!p.waitForStarted(3000)) return out;
        p.write(text.toUtf8());
        p.closeWriteChannel();
        if (!p.waitForFinished(5000)) {
            p.kill();  // our own child, by handle
            p.waitForFinished(1000);
            return out;
        }
        // A formatter that failed says so on stderr and prints nothing useful; taking
        // its stdout anyway would REPLACE THE WHOLE FILE with an error message.
        if (p.exitCode() != 0) return out;
        const QString formatted = QString::fromUtf8(p.readAllStandardOutput());
        if (formatted.isEmpty() || formatted == text) return out;

        // One edit spanning the whole document. Line-diffing it would be prettier and
        // is a great way to corrupt a file over an off-by-one.
        const QStringList lines = splitLines(text);
        out.append(QJsonObject{
            {"range", range(0, 0, lines.size(), 0)},
            {"newText", formatted}});
        return out;
    }

    // ---- dispatch --------------------------------------------------------

    void handle(const QJsonObject& msg) {
        const QString method = msg.value(QStringLiteral("method")).toString();
        const QJsonValue id = msg.value(QStringLiteral("id"));
        const QJsonObject params = msg.value(QStringLiteral("params")).toObject();
        const bool isRequest = !id.isNull() && !id.isUndefined();

        if (method == QLatin1String("initialize")) {
            rootPath_ = params.value(QStringLiteral("rootUri")).isString()
                            ? uriToPath(params.value(QStringLiteral("rootUri")).toString())
                            : params.value(QStringLiteral("rootPath")).toString();
            if (!rootPath_.isEmpty() && QDir(rootPath_).exists()) {
                // Anything this server shells out to resolves against the editor's
                // project root, not the process cwd — which, for a server the editor
                // spawned, could be anywhere at all (often /).
                Tools::setThreadRoot(rootPath_);
            }
            logMsg(QStringLiteral("initialize · root=%1 · %2/%3")
                       .arg(rootPath_.isEmpty() ? QStringLiteral("(none)") : rootPath_,
                            backendId(), model()));

            // Advertise EXACTLY what is implemented, and nothing else. The PHP server
            // claimed definitionProvider and then never handled
            // textDocument/definition, so go-to-definition in an editor did nothing
            // at all and looked like a broken editor rather than a lying server.
            const QJsonObject caps{
                // Full sync (1): didChange carries the whole document. Incremental
                // sync would be a second, subtly different copy of the buffer to keep
                // correct, and the linters need the whole text anyway.
                {"textDocumentSync", QJsonObject{{"openClose", true}, {"change", 1}, {"save", true}}},
                {"completionProvider", QJsonObject{{"resolveProvider", false}}},
                {"hoverProvider", true},
                {"definitionProvider", true},
                {"documentSymbolProvider", true},
                {"referencesProvider", true},
                {"renameProvider", true},
                {"documentFormattingProvider", true},
            };
            reply(id, QJsonObject{
                         {"capabilities", caps},
                         {"serverInfo", QJsonObject{{"name", "OllamaDev"},
                                                    {"version", ODV_VERSION}}},
                     });
            return;
        }

        if (method == QLatin1String("initialized")) return;  // notification, nothing to do

        if (method == QLatin1String("shutdown")) {
            shutdownRequested_ = true;
            reply(id, QJsonValue(QJsonValue::Null));  // must be null, and must arrive before exit
            return;
        }
        if (method == QLatin1String("exit")) {
            exit_ = true;
            return;
        }

        if (method == QLatin1String("textDocument/didOpen")) {
            const QJsonObject td = params.value(QStringLiteral("textDocument")).toObject();
            const QString uri = td.value(QStringLiteral("uri")).toString();
            Doc d;
            d.text = td.value(QStringLiteral("text")).toString();
            d.version = td.value(QStringLiteral("version")).toInt();
            docs_.insert(uri, d);
            publishDiagnostics(uri);
            return;
        }

        if (method == QLatin1String("textDocument/didChange")) {
            const QJsonObject td = params.value(QStringLiteral("textDocument")).toObject();
            const QString uri = td.value(QStringLiteral("uri")).toString();
            const QJsonArray changes = params.value(QStringLiteral("contentChanges")).toArray();
            if (changes.isEmpty()) return;
            // Full sync: the LAST change holds the whole document. Taking the first
            // would silently drop everything the client batched after it.
            Doc d = docs_.value(uri);
            d.text = changes.last().toObject().value(QStringLiteral("text")).toString();
            d.version = td.value(QStringLiteral("version")).toInt();
            docs_.insert(uri, d);
            publishDiagnostics(uri);
            return;
        }

        if (method == QLatin1String("textDocument/didClose")) {
            const QString uri = params.value(QStringLiteral("textDocument"))
                                    .toObject()
                                    .value(QStringLiteral("uri"))
                                    .toString();
            docs_.remove(uri);
            // Clear the file's squiggles: they belong to a buffer that no longer exists.
            notify(QStringLiteral("textDocument/publishDiagnostics"),
                   QJsonObject{{"uri", uri}, {"diagnostics", QJsonArray()}});
            return;
        }

        if (method == QLatin1String("textDocument/didSave")) {
            // The client MAY send the saved text; when it does not, the buffer we
            // already hold is the same text — it was just written to disk. Re-lint
            // either way: saving is exactly when a compiler-backed diagnostic becomes
            // worth re-running.
            const QString uri = params.value(QStringLiteral("textDocument"))
                                    .toObject()
                                    .value(QStringLiteral("uri"))
                                    .toString();
            const QJsonValue text = params.value(QStringLiteral("text"));
            if (text.isString() && docs_.contains(uri)) {
                Doc d = docs_.value(uri);
                d.text = text.toString();
                docs_.insert(uri, d);
            }
            if (docs_.contains(uri)) publishDiagnostics(uri);
            return;
        }

        if (!isRequest) return;  // an unknown NOTIFICATION is simply ignored, per spec

        if (method == QLatin1String("textDocument/documentSymbol")) {
            reply(id, documentSymbol(params));
            return;
        }
        if (method == QLatin1String("textDocument/references")) {
            reply(id, references(params));
            return;
        }
        if (method == QLatin1String("textDocument/rename")) {
            reply(id, rename(params));
            return;
        }
        if (method == QLatin1String("textDocument/formatting")) {
            reply(id, formatting(params));
            return;
        }

        if (method == QLatin1String("textDocument/completion")) {
            reply(id, completion(params));
            return;
        }
        if (method == QLatin1String("textDocument/hover")) {
            reply(id, hover(params));
            return;
        }
        if (method == QLatin1String("textDocument/definition")) {
            reply(id, definition(params));
            return;
        }

        // An unanswered REQUEST hangs the editor's UI, so unknown methods get a real
        // JSON-RPC error rather than silence.
        replyError(id, -32601, QStringLiteral("method not found: %1").arg(method));
    }

    QString backendId() const {
        if (!o_.backend.isEmpty()) return o_.backend;
        return Config::str(QStringLiteral("model.backend"), QStringLiteral("ollama"));
    }

    QString model() {
        if (!model_.isEmpty()) return model_;
        model_ = o_.model;
        if (model_.isEmpty()) {
            auto b = Backends::get(backendId());
            if (b) model_ = b->defaultModel();
        }
        return model_;
    }

    Transport* t_;
    LspOptions o_;
    QString model_;
    QHash<QString, Doc> docs_;
    QString rootPath_;
    bool shutdownRequested_ = false;
    bool exit_ = false;
};

}  // namespace

int LspServer::serve(const LspOptions& o) {
    // An LSP client is a remote caller, exactly like an MCP client: READ-ONLY and
    // non-interactive. Completion never runs a tool (we call the backend directly),
    // but the registry is loaded, and nothing here may ever block on an approval
    // prompt that no human is watching.
    Permission::setMode(PermMode::ReadOnly);
    Permission::setInteractive(false);
    Tools::registerAll();

    if (o.port > 0) {
        QTcpServer srv;
        // Loopback ONLY. Binding this to 0.0.0.0 would hand anyone on the network a
        // server that reads local files and drives the user's model.
        if (!srv.listen(QHostAddress::LocalHost, static_cast<quint16>(o.port))) {
            logMsg(QStringLiteral("cannot listen on 127.0.0.1:%1 — %2")
                       .arg(o.port)
                       .arg(srv.errorString()));
            return 1;
        }
        logMsg(QStringLiteral("listening on 127.0.0.1:%1").arg(srv.serverPort()));

        // One editor at a time, served to completion. A second client would be a
        // second view of the same document store, which is not something LSP means.
        while (srv.waitForNewConnection(-1)) {
            QTcpSocket* sock = srv.nextPendingConnection();
            if (!sock) continue;
            SocketTransport tr(sock);
            Server(&tr, o).run();
            sock->disconnectFromHost();
            // Plain delete, not deleteLater(): there is no event loop spinning here to
            // ever deliver the deferred deletion, so deleteLater() would leak one
            // socket per editor session.
            delete sock;
            logMsg(QStringLiteral("client disconnected"));
        }
        return 0;
    }

    StdioTransport tr;
    if (!tr.ok()) {
        logMsg(QStringLiteral("cannot take over stdout"));
        return 1;
    }
    logMsg(QStringLiteral("stdio mode · OllamaDev %1").arg(ODV_VERSION));
    return Server(&tr, o).run();
}

}  // namespace odv
