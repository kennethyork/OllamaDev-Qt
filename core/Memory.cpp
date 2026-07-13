#include "Memory.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>

#include "Backend.h"
#include "Config.h"
#include "Json.h"
#include "Tools.h"

namespace odv {
namespace {

QString readText(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

// QSaveFile is temp-file + atomic rename: the desktop's graph view can be reading
// the memory dir while an agent writes a note, and must never see a half file.
bool writeText(const QString& path, const QString& text) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(text.toUtf8());
    return f.commit();
}

QString homeRoot() {
    const QString h = qEnvironmentVariable("HOME");
    return h.isEmpty() ? QDir::tempPath() : h;
}

// Thread-local for a crew coder (its sandbox), the process cwd otherwise — the
// same rule the tools use, so a coder's notes are its own.
QString projectRoot() { return Tools::threadRoot(); }

MemoryNote parseNote(const QString& path, const QString& slug) {
    const QString content = readText(path);
    MemoryNote m;
    m.slug = slug;
    m.title = slug;
    m.path = path;
    m.body = content;

    static const QRegularExpression fm(QStringLiteral("^---\\s*\\n(.*?)\\n---\\s*\\n"),
                                       QRegularExpression::DotMatchesEverythingOption);
    const auto match = fm.match(content);
    if (match.hasMatch()) {
        m.body = content.mid(match.capturedEnd(0));
        const auto lines = match.captured(1).split('\n');
        for (const QString& line : lines) {
            const int colon = line.indexOf(':');
            if (colon < 0) continue;
            const QString key = line.left(colon).trimmed().toLower();
            QString val = line.mid(colon + 1).trimmed();
            while (!val.isEmpty() && (val.startsWith('"') || val.startsWith('\''))) val.remove(0, 1);
            while (!val.isEmpty() && (val.endsWith('"') || val.endsWith('\''))) val.chop(1);
            if (key == QLatin1String("title") && !val.isEmpty()) m.title = val;
            else if (key == QLatin1String("tags"))
                m.tags = val.split(QRegularExpression(QStringLiteral("[,;]")), Qt::SkipEmptyParts);
        }
    }
    for (QString& t : m.tags) t = t.trimmed();
    m.tags.removeAll(QString());

    static const QRegularExpression link(QStringLiteral("\\[\\[([^\\]]+)\\]\\]"));
    auto it = link.globalMatch(m.body);
    while (it.hasNext()) {
        const QString target = it.next().captured(1).trimmed();
        if (!target.isEmpty() && !m.links.contains(target)) m.links << target;
    }
    if (m.title.isEmpty()) m.title = slug;
    return m;
}

}  // namespace

QStringList Memory::baseDirs() {
    QStringList d{projectDir(), homeRoot() + QStringLiteral("/.ollamadev/memory")};
    d.removeDuplicates();
    return d;
}

QString Memory::projectDir() { return projectRoot() + QStringLiteral("/.ollamadev/memory"); }

QVector<MemoryNote> Memory::all() {
    QVector<MemoryNote> out;
    QStringList seen;  // the project dir comes first, so it wins
    for (const QString& base : baseDirs()) {
        QDir bd(base);
        if (!bd.exists()) continue;
        const auto files = bd.entryList({QStringLiteral("*.md")}, QDir::Files, QDir::Name);
        for (const QString& f : files) {
            const QString slug = QFileInfo(f).completeBaseName();
            if (seen.contains(slug)) continue;
            seen << slug;
            out.append(parseNote(base + QLatin1Char('/') + f, slug));
        }
    }
    std::sort(out.begin(), out.end(),
              [](const MemoryNote& a, const MemoryNote& b) { return a.slug < b.slug; });
    return out;
}

MemoryNote Memory::get(const QString& slugOrTitle) {
    const QVector<MemoryNote> notes = all();
    for (const MemoryNote& m : notes)
        if (m.slug == slugOrTitle) return m;
    // A model that read the index will often refer to a note by its TITLE.
    for (const MemoryNote& m : notes)
        if (m.title.compare(slugOrTitle, Qt::CaseInsensitive) == 0) return m;
    return {};
}

QString Memory::slugify(const QString& s) {
    static const QRegularExpression nonAlnum(QStringLiteral("[^a-zA-Z0-9]+"));
    QString out = s.toLower();
    out.replace(nonAlnum, QStringLiteral("-"));
    out = out.left(48);
    while (out.startsWith('-')) out.remove(0, 1);
    while (out.endsWith('-')) out.chop(1);
    return out.isEmpty() ? QStringLiteral("note") : out;
}

QString Memory::save(const QString& title, const QString& body, const QStringList& tags,
                     const QString& slug) {
    const QString s = slugify(slug.isEmpty() ? title : slug);

    QString fm = QStringLiteral("---\ntitle: %1\n").arg(title.trimmed());
    if (!tags.isEmpty()) {
        QStringList clean;
        for (const QString& t : tags)
            if (!t.trimmed().isEmpty()) clean << t.trimmed();
        if (!clean.isEmpty())
            fm += QStringLiteral("tags: %1\n").arg(clean.join(QStringLiteral(", ")));
    }
    fm += QStringLiteral("---\n\n");

    QString b = body;
    while (b.endsWith('\n') || b.endsWith(' ')) b.chop(1);

    const QString path = projectDir() + QLatin1Char('/') + s + QStringLiteral(".md");
    writeText(path, fm + b + QStringLiteral("\n"));
    return s;
}

bool Memory::remove(const QString& slug) {
    const MemoryNote m = get(slug);
    if (m.isNull()) return false;
    return QFile::remove(m.path);
}

QVector<MemoryNote> Memory::search(const QString& query) {
    const QString q = query.trimmed().toLower();
    const QVector<MemoryNote> notes = all();
    if (q.isEmpty()) return notes;

    QVector<MemoryNote> hits;
    for (const MemoryNote& m : notes) {
        const QString hay =
            (m.title + QLatin1Char(' ') + m.tags.join(QLatin1Char(' ')) + QLatin1Char(' ') + m.body)
                .toLower();
        if (hay.contains(q)) hits.append(m);
    }
    return hits;
}

QString Memory::index(int cap) {
    const QVector<MemoryNote> notes = all();
    QStringList lines;
    for (const MemoryNote& m : notes) {
        if (lines.size() >= cap) {
            lines << QStringLiteral("- … (%1 more — search with recall)")
                         .arg(notes.size() - cap);
            break;
        }
        lines << (m.title == m.slug ? QStringLiteral("- %1").arg(m.slug)
                                    : QStringLiteral("- %1 — %2").arg(m.slug, m.title));
    }
    return lines.join('\n');
}

QJsonObject Memory::graph() {
    const QVector<MemoryNote> notes = all();

    QHash<QString, QString> toSlug;  // lowercase slug OR title -> slug
    for (const MemoryNote& m : notes) {
        toSlug.insert(m.slug.toLower(), m.slug);
        toSlug.insert(m.title.toLower(), m.slug);
    }

    QJsonArray edges;
    QHash<QString, int> degree;
    QStringList seen;
    for (const MemoryNote& m : notes) {
        for (const QString& link : m.links) {
            // A link to a note that does not exist is a dangling reference, not an
            // edge — the graph only ever shows real notes.
            const QString target = toSlug.value(link.trimmed().toLower());
            if (target.isEmpty() || target == m.slug) continue;
            const QString key = m.slug + QStringLiteral("->") + target;
            if (seen.contains(key)) continue;
            seen << key;
            edges.append(QJsonObject{{"from", m.slug}, {"to", target}});
            ++degree[m.slug];
            ++degree[target];
        }
    }

    QJsonArray nodes;
    for (const MemoryNote& m : notes) {
        QJsonArray tags;
        for (const QString& t : m.tags) tags.append(t);
        nodes.append(QJsonObject{{"id", m.slug},
                                 {"title", m.title},
                                 {"tags", tags},
                                 {"degree", degree.value(m.slug, 0)}});
    }
    return QJsonObject{{"nodes", nodes}, {"edges", edges}};
}

QStringList Memory::autoRemember(const QString& context, const QString& backendId,
                                 const QString& model) {
    const QString ctx = context.trimmed();
    if (ctx.isEmpty()) return {};

    const BackendPtr be =
        Backends::get(backendId.isEmpty() ? Config::str(QStringLiteral("model.backend"),
                                                        QStringLiteral("ollama"))
                                          : backendId);
    if (!be) return {};
    QString m = model;
    if (m.isEmpty()) m = Config::str(QStringLiteral("ollama.defaultModel"));
    if (m.isEmpty()) m = be->defaultModel();
    if (m.isEmpty()) return {};

    QVector<MemoryNote> have = all();
    QStringList existing;
    for (const MemoryNote& n : have)
        existing << QStringLiteral("%1 (%2)").arg(n.slug, n.title);

    const QString sys = QStringLiteral(
        "Extract DURABLE, reusable facts about THIS PROJECT worth remembering across sessions — "
        "architecture, conventions, key decisions, gotchas, where things live. NOT transient task "
        "details or chatter. Skip anything already covered by the existing notes. Output ONLY "
        "JSON: {\"notes\":[{\"title\":\"short title\",\"body\":\"1-3 sentences; link related "
        "notes with [[slug]]\"}]} — at most 4, or an empty array if nothing durable is worth "
        "saving.");

    QString usr;
    if (!existing.isEmpty())
        usr += QStringLiteral("Existing notes (do NOT duplicate):\n- %1\n\n")
                   .arg(existing.mid(0, 40).join(QStringLiteral("\n- ")));
    usr += QStringLiteral("Recent work:\n%1\n\nExtract durable facts worth keeping.")
               .arg(ctx.left(6000));

    CancelToken cancel;
    const QJsonObject j = be->chatJson(m,
                                       {{"system", sys, {}, {}, {}, {}, {}},
                                        {"user", usr, {}, {}, {}, {}, {}}},
                                       cancel);

    QStringList saved;
    const QJsonArray notes = j.value(QStringLiteral("notes")).toArray();
    for (const QJsonValue& v : notes) {
        if (saved.size() >= 4) break;
        const QJsonObject o = v.toObject();
        const QString title = o.value(QStringLiteral("title")).toString().trimmed();
        const QString body = o.value(QStringLiteral("body")).toString().trimmed();
        if (title.isEmpty() || body.isEmpty()) continue;

        const QString slug = slugify(title);
        bool dupe = false;
        for (const MemoryNote& n : have)
            if (n.slug == slug || n.title.compare(title, Qt::CaseInsensitive) == 0) dupe = true;
        if (dupe) continue;

        save(title, body);
        saved << slug;

        // Dedupe within this batch too — a model asked for 4 notes will happily
        // return the same fact twice, phrased differently.
        MemoryNote fresh;
        fresh.slug = slug;
        fresh.title = title;
        have.append(fresh);
    }
    return saved;
}

}  // namespace odv
