#include "Json.h"

#include <QJsonParseError>
#include <QStringList>

namespace odv {
namespace json {
namespace {

// Bounds the balanced-scan retry loop. Pathological input ("{{{{{{...") would
// otherwise make the scan quadratic; a model that needs more than a handful of
// candidate openers is not going to yield JSON on the next one either.
constexpr int kMaxScanAttempts = 16;

// Drop a surrounding ``` / ```json fence. Only the OUTERMOST fence goes: a JSON
// string may legitimately contain backticks, so anything inside the payload is
// left for the parser to deal with.
QString stripFence(const QString& in) {
    QString t = in.trimmed();
    if (!t.startsWith(QLatin1String("```"))) return t;
    const int nl = t.indexOf(u'\n');
    if (nl < 0) return t;  // a lone "```json" with no body — nothing to unwrap
    QString body = t.mid(nl + 1);
    const int close = body.lastIndexOf(QLatin1String("```"));
    if (close >= 0) body = body.left(close);
    return body.trimmed();
}

// Index of the brace/bracket closing the one at `start`, or -1 if unbalanced.
// String literals are skipped whole, so a `{` or `"` inside a JSON string never
// moves the depth counter — that is the difference between this and a naive
// character count.
int matchingEnd(const QString& s, int start) {
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (int i = start; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (inString) {
            if (escaped) escaped = false;
            else if (c == u'\\') escaped = true;
            else if (c == u'"') inString = false;
            continue;
        }
        if (c == u'"') {
            inString = true;
        } else if (c == u'{' || c == u'[') {
            ++depth;
        } else if (c == u'}' || c == u']') {
            if (--depth == 0) return i;
            if (depth < 0) return -1;  // a stray closer before ours: not our payload
        }
    }
    return -1;
}

// Last resort: the model wrote prose around (or between) the JSON. Walk the
// openers left to right and return the first balanced span that actually parses.
QJsonDocument scanBalanced(const QString& s) {
    int attempts = 0;
    for (int i = 0; i < s.size() && attempts < kMaxScanAttempts; ++i) {
        const QChar c = s.at(i);
        if (c != u'{' && c != u'[') continue;
        ++attempts;
        const int end = matchingEnd(s, i);
        if (end < 0) continue;
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(s.mid(i, end - i + 1).toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && !doc.isNull()) return doc;
    }
    return {};
}

QJsonDocument parseStrict(const QString& s) {
    if (s.isEmpty()) return {};
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(s.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || doc.isNull()) return {};
    return doc;
}

// Insert `v` at keys[idx..] inside `o`, materialising intermediate objects.
void setPath(QJsonObject& o, const QStringList& keys, int idx, const QJsonValue& v) {
    const QString& k = keys.at(idx);
    if (idx == keys.size() - 1) {
        o.insert(k, v);
        return;
    }
    // A non-object sitting at an intermediate key loses: the dotted key is the
    // more specific statement of intent, and the parent scalar was a stale value.
    QJsonObject child = o.value(k).toObject();
    setPath(child, keys, idx + 1, v);
    o.insert(k, child);
}

}  // namespace

QJsonDocument decodeLoose(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return {};

    const QString unfenced = stripFence(trimmed);

    if (QJsonDocument doc = parseStrict(unfenced); !doc.isNull()) return doc;
    if (unfenced != trimmed) {
        if (QJsonDocument doc = parseStrict(trimmed); !doc.isNull()) return doc;
    }

    if (QJsonDocument doc = scanBalanced(unfenced); !doc.isNull()) return doc;
    if (unfenced != trimmed) {
        if (QJsonDocument doc = scanBalanced(trimmed); !doc.isNull()) return doc;
    }
    return {};
}

QJsonObject objectFrom(const QString& text) {
    const QJsonDocument doc = decodeLoose(text);
    if (doc.isObject()) return doc.object();
    return {};
}

QByteArray encode(const QJsonObject& o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QByteArray encode(const QJsonArray& a) {
    return QJsonDocument(a).toJson(QJsonDocument::Compact);
}

QJsonValue at(const QJsonObject& root, const QString& dottedKey, const QJsonValue& fallback) {
    if (dottedKey.isEmpty()) return fallback;
    const QStringList keys = dottedKey.split(u'.', Qt::SkipEmptyParts);
    if (keys.isEmpty()) return fallback;

    QJsonValue cur = QJsonValue(root);
    for (const QString& k : keys) {
        if (!cur.isObject()) return fallback;
        const QJsonObject o = cur.toObject();
        if (!o.contains(k)) return fallback;
        cur = o.value(k);
    }
    // An explicit null is a value the caller stored, not a missing key; but a
    // caller asking for a fallback wants something usable, so null yields it.
    return cur.isUndefined() || cur.isNull() ? fallback : cur;
}

QJsonObject mergeDeep(const QJsonObject& base, const QJsonObject& overlay) {
    QJsonObject out = base;
    for (auto it = overlay.constBegin(); it != overlay.constEnd(); ++it) {
        const QJsonValue existing = out.value(it.key());
        if (existing.isObject() && it.value().isObject()) {
            out.insert(it.key(), mergeDeep(existing.toObject(), it.value().toObject()));
        } else {
            // Arrays overwrite rather than concatenate: a user who sets
            // ollama.hosts means "these hosts", not "these plus the defaults".
            out.insert(it.key(), it.value());
        }
    }
    return out;
}

QJsonObject expandDotted(const QJsonObject& flat) {
    QJsonObject out;
    for (auto it = flat.constBegin(); it != flat.constEnd(); ++it) {
        const QStringList keys = it.key().split(u'.', Qt::SkipEmptyParts);
        if (keys.isEmpty()) continue;
        setPath(out, keys, 0, it.value());
    }
    return out;
}

}  // namespace json
}  // namespace odv
