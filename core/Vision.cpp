#include "Vision.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace odv {
namespace {

// Extensions we treat as attachable images. Anything else is left as plain text
// so a stray "@notes.txt" is never mistaken for a picture.
const QStringList& imageExts() {
    static const QStringList e{QStringLiteral("png"),  QStringLiteral("jpg"),
                               QStringLiteral("jpeg"), QStringLiteral("gif"),
                               QStringLiteral("webp"), QStringLiteral("bmp")};
    return e;
}

// Strip a single layer of matching surrounding quotes, so `/image "a b.png"`
// resolves (a path with a space is common on desktops).
QString unquote(QString s) {
    s = s.trimmed();
    if (s.size() >= 2) {
        const QChar f = s.front(), l = s.back();
        if ((f == '"' && l == '"') || (f == '\'' && l == '\'')) s = s.mid(1, s.size() - 2);
    }
    return s;
}

// ~ expands to the home dir; a relative path is left for QFileInfo to resolve
// against the current working directory (which in the REPL is the project root).
QString resolvePath(const QString& raw) {
    QString p = unquote(raw);
    if (p == QLatin1String("~"))
        p = QDir::homePath();
    else if (p.startsWith(QLatin1String("~/")))
        p = QDir::homePath() + p.mid(1);
    return p;
}

// Does this path look like a readable image? Extension gates first, then a
// magic-byte sniff so a mislabelled file (a .png that is really HTML) is skipped
// before we waste base64 budget on it. A recognised extension with an
// unrecognised header is still attached — some valid encoders write odd headers,
// and Ollama simply ignores an image it cannot decode.
bool isImage(const QString& path) {
    if (path.isEmpty()) return false;
    QFileInfo fi(path);
    if (!fi.isFile() || !fi.isReadable()) return false;
    if (!imageExts().contains(fi.suffix().toLower())) return false;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = f.read(12);
    f.close();
    if (head.isEmpty()) return false;

    if (head.startsWith(QByteArray("\x89PNG", 4))) return true;         // PNG
    if (head.startsWith(QByteArray("\xFF\xD8\xFF", 3))) return true;    // JPEG
    if (head.startsWith("GIF8")) return true;                            // GIF
    if (head.startsWith("BM")) return true;                              // BMP
    if (head.size() >= 12 && head.left(4) == "RIFF" && head.mid(8, 4) == "WEBP") return true;
    return true;  // recognised ext, unrecognised header: attach anyway
}

// A leading `/image <path> [rest]` command. Captures the path and the trailing
// prompt separately.
const QRegularExpression& imageCmd() {
    static const QRegularExpression re(QStringLiteral("^/image\\s+(\\S+)\\s*(.*)$"),
                                       QRegularExpression::DotMatchesEverythingOption);
    return re;
}

// `@<path>` tokens anywhere in the text. (?<![\w@]) skips emails (user@host) and
// a doubled @@, matching the @file-mention rule in the REPL so the two agree on
// what a token is.
const QRegularExpression& atToken() {
    static const QRegularExpression re(QStringLiteral("(?<![\\w@])@([^\\s@]+)"));
    return re;
}

}  // namespace

QStringList Vision::extractImagePaths(const QString& prompt) {
    QStringList paths;

    const auto m = imageCmd().match(prompt.trimmed());
    if (m.hasMatch()) {
        const QString p = resolvePath(m.captured(1));
        if (isImage(p)) paths << p;
        return paths;  // the /image form carries exactly one attachment
    }

    auto it = atToken().globalMatch(prompt);
    while (it.hasNext()) {
        const QString p = resolvePath(it.next().captured(1));
        if (isImage(p) && !paths.contains(p)) paths << p;
    }
    return paths;
}

QString Vision::stripImageTokens(const QString& prompt) {
    const auto m = imageCmd().match(prompt.trimmed());
    if (m.hasMatch()) {
        const QString rest = m.captured(2).trimmed();
        // With the picture removed the model still needs an instruction, so a bare
        // `/image foo.png` becomes an explicit ask rather than an empty message.
        return rest.isEmpty() ? QStringLiteral("Describe this image.") : rest;
    }

    // Replace each @token that IS an image with its bare filename, so the prompt
    // reads naturally on a non-vision model; leave non-image tokens untouched so
    // the @file-mention path can still inline them.
    QString out;
    int last = 0;
    auto it = atToken().globalMatch(prompt);
    while (it.hasNext()) {
        const auto match = it.next();
        const QString p = resolvePath(match.captured(1));
        out += prompt.mid(last, match.capturedStart() - last);
        if (isImage(p))
            out += QFileInfo(p).fileName();
        else
            out += match.captured(0);  // not an image: keep the raw @token
        last = match.capturedEnd();
    }
    out += prompt.mid(last);
    return out.trimmed();
}

QString Vision::encodeBase64(const QString& path, QString* err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("cannot read %1").arg(path);
        return {};
    }
    const QByteArray data = f.readAll();
    f.close();
    if (data.isEmpty()) {
        if (err) *err = QStringLiteral("empty file %1").arg(path);
        return {};
    }
    return QString::fromLatin1(data.toBase64());
}

}  // namespace odv
