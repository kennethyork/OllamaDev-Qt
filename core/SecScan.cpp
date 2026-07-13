#include "SecScan.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

namespace odv {
namespace {

struct Rule {
    const char* id;
    const char* severity;  // high|med|low
    const char* pattern;
    bool caseInsensitive;
};

// Tight patterns, anchored on the credential's own shape (prefix + exact length)
// wherever the issuer gives us one. The generic key/password rule is the only
// loose one, hence its lower severity.
const QVector<Rule>& rules() {
    static const QVector<Rule> r{
        {"aws-akid", "high", R"(\bAKIA[0-9A-Z]{16}\b)", false},
        {"aws-secret", "high",
         R"(\baws_secret_access_key\b\s*[=:]\s*['"][A-Za-z0-9/+]{40}['"])", true},
        {"gcp-key", "high", R"(\bAIza[0-9A-Za-z\-_]{35}\b)", false},
        {"github-token", "high",
         R"(\b(?:ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9]{36}\b|\bgithub_pat_[A-Za-z0-9_]{82}\b)", false},
        {"slack-token", "high", R"(\bxox[baprs]-[A-Za-z0-9-]{10,}\b)", false},
        {"stripe-key", "high", R"(\bsk_live_[A-Za-z0-9]{16,}\b)", false},
        {"openai-key", "high",
         R"(\bsk-[A-Za-z0-9]{20}T3BlbkFJ[A-Za-z0-9]{20}\b|\bsk-proj-[A-Za-z0-9_-]{20,}\b)", false},
        {"private-key", "high", R"(-----BEGIN (?:RSA |EC |OPENSSH |DSA |PGP )?PRIVATE KEY-----)",
         false},
        {"jwt", "med", R"(\beyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\b)",
         false},
        {"basic-auth", "high", R"(\b[a-z][a-z0-9+.\-]*://[^/\s:@]+:[^/\s:@]+@)", true},
        {"generic-secret", "med",
         R"(\b(?:api[_-]?key|secret|token|passwd|password|access[_-]?key|private[_-]?key)\b\s*[=:]\s*['"][^'"\s]{8,}['"])",
         true},
        {"php-eval", "med", R"(\beval\s*\(\s*\$)", false},
        {"shell-var", "med",
         R"(\b(?:shell_exec|exec|system|passthru|popen|proc_open)\s*\(\s*[^'")]*\$)", false},
    };
    return r;
}

// Compiled once — the crew gate scans whole diffs line by line, and recompiling
// thirteen PCRE2 patterns per line dominated the PHP version's scan time.
const QVector<QRegularExpression>& compiled() {
    static const QVector<QRegularExpression> res = [] {
        QVector<QRegularExpression> out;
        out.reserve(rules().size());
        for (const Rule& r : rules()) {
            QRegularExpression re(QString::fromLatin1(r.pattern));
            if (r.caseInsensitive) {
                re.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
            }
            re.optimize();
            out.push_back(re);
        }
        return out;
    }();
    return res;
}

// Docs, .env templates and obvious dummies are not leaks. Flagging them trains
// people to ignore findings, which is how a real one gets waved through.
bool isPlaceholder(const QString& line) {
    static const QRegularExpression re(
        R"(\b(example|placeholder|dummy|your[_-]?(?:key|token|secret)|xxx+|<[^>]+>|changeme|redacted|\*{4,}|\.\.\.\.))",
        QRegularExpression::CaseInsensitiveOption);
    return re.match(line).hasMatch();
}

// The security boundary of this whole file. A finding is shown to humans, written
// to disk and pasted into auditor prompts, so it must be useless to an attacker
// who reads it: at most the first 4 and last 2 characters survive, and anything
// short enough that those would give the secret away is masked outright.
QString redact(const QString& match) {
    const QString s = match.trimmed();
    if (s.isEmpty()) return {};
    if (s.size() <= 10) return QString(s.size(), u'•');
    return s.left(4) + QString(QChar(u'…')) + s.right(2);
}

QStringList splitLines(const QString& text) {
    static const QRegularExpression nl(QStringLiteral(R"(\r\n|\r|\n)"));
    return text.split(nl);
}

// One line, one pass over the rules. At most one finding per rule per line: a
// second hit from the same rule on the same line tells a reviewer nothing new.
void scanLine(const QString& line, int lineNo, QVector<Finding>& out) {
    if (line.isEmpty() || isPlaceholder(line)) return;
    const QVector<QRegularExpression>& res = compiled();
    for (int i = 0; i < rules().size(); ++i) {
        const QRegularExpressionMatch m = res.at(i).match(line);
        if (!m.hasMatch()) continue;
        out.push_back(Finding{QString::fromLatin1(rules().at(i).id),
                              QString::fromLatin1(rules().at(i).severity), lineNo,
                              redact(m.captured(0))});
    }
}

}  // namespace

QVector<Finding> SecScan::scanText(const QString& text) {
    QVector<Finding> out;
    const QStringList lines = splitLines(text);
    for (int i = 0; i < lines.size(); ++i) {
        scanLine(lines.at(i), i + 1, out);
    }
    return out;
}

QVector<Finding> SecScan::scanDiff(const QString& diff) {
    static const QRegularExpression hunk(QStringLiteral(R"(^@@ -\d+(?:,\d+)? \+(\d+))"));

    QVector<Finding> out;
    int lineNo = 0;
    for (const QString& line : splitLines(diff)) {
        if (line.startsWith(QLatin1String("+++ "))) continue;  // file header, not content

        const QRegularExpressionMatch h = hunk.match(line);
        if (h.hasMatch()) {
            lineNo = h.captured(1).toInt() - 1;
            continue;
        }

        if (line.startsWith(u'+')) {
            ++lineNo;
            QVector<Finding> hits;
            scanLine(line.mid(1), lineNo, hits);
            out += hits;
        } else if (line.isEmpty() || line.startsWith(u' ')) {
            ++lineNo;  // context line: advances the new file, introduces nothing
        }
        // '-' lines are removals — the point is to flag what a change ADDS.
    }
    return out;
}

QVector<Finding> SecScan::scanFile(const QString& path) {
    QFileInfo info(path);
    if (!info.isFile() || info.size() > 2000000) return {};

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray data = f.readAll();
    f.close();
    if (data.isEmpty()) return {};

    // A NUL early in the file means binary: no credential worth reading is in
    // there, and the regexes would burn time on noise.
    if (data.left(8000).contains('\0')) return {};

    return scanText(QString::fromUtf8(data));
}

}  // namespace odv
