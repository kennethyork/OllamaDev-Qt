#include "Skills.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUrl>
#include <algorithm>

#include "Config.h"
#include "Json.h"
#include "Tools.h"

namespace odv {
namespace {

// ------------------------------------------------------------ small file I/O

QString readText(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

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

// The project a skill lookup is relative to. For a crew coder this is its
// SANDBOX (Tools::threadRoot() is thread-local and set to the sandbox), which is
// precisely how a materialised starter becomes discoverable to that coder and to
// nobody else. Falls back to the process cwd for the plain CLI.
QString projectRoot() { return Tools::threadRoot(); }

// ---------------------------------------------------------------- frontmatter

const QRegularExpression& frontMatterRx() {
    static const QRegularExpression rx(QStringLiteral("^---\\s*\\n(.*?)\\n---\\s*\\n"),
                                       QRegularExpression::DotMatchesEverythingOption);
    return rx;
}

// Trim the quoting a hand-written YAML value tends to carry.
QString unquote(const QString& s) {
    QString t = s.trimmed();
    while (!t.isEmpty() && (t.startsWith('"') || t.startsWith('\'') || t.startsWith(' ')))
        t.remove(0, 1);
    while (!t.isEmpty() && (t.endsWith('"') || t.endsWith('\'') || t.endsWith(' ')))
        t.chop(1);
    return t;
}

// Parse a SKILL.md into name/description/body. `fallback` (the folder name) is
// the name when the frontmatter omits one, so a skill folder is never nameless.
Skill parseSkillMd(const QString& path, const QString& fallback) {
    const QString content = readText(path);
    Skill s;
    s.name = fallback;
    s.body = content;

    const auto m = frontMatterRx().match(content);
    if (m.hasMatch()) {
        s.body = content.mid(m.capturedEnd(0));
        const auto lines = m.captured(1).split('\n');
        for (const QString& line : lines) {
            const int colon = line.indexOf(':');
            if (colon < 0) continue;
            const QString key = line.left(colon).trimmed().toLower();
            const QString val = unquote(line.mid(colon + 1));
            if (key == QLatin1String("name") && !val.isEmpty()) s.name = val;
            else if (key == QLatin1String("description")) s.description = val;
        }
    }
    // No description? Use the first non-heading line of the body, so a skill
    // written without frontmatter still says something useful in the catalog.
    if (s.description.isEmpty()) {
        const auto lines = s.body.split('\n');
        for (const QString& line : lines) {
            const QString t = line.trimmed();
            if (!t.isEmpty() && !t.startsWith('#')) {
                s.description = t;
                break;
            }
        }
    }
    if (s.name.isEmpty()) s.name = fallback;
    return s;
}

// Every directory containing a SKILL.md at or under `base`, depth-limited so a
// pathological archive cannot walk us into a deep tree.
QStringList findSkillDirs(const QString& base, int depth = 4) {
    QDir d(base);
    if (!d.exists()) return {};
    if (QFileInfo::exists(base + QStringLiteral("/SKILL.md"))) return {base};
    if (depth <= 0) return {};
    QStringList found;
    const auto subs = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& sub : subs)
        found += findSkillDirs(base + QLatin1Char('/') + sub, depth - 1);
    return found;
}

// Plain recursive copy. Deliberately NOT Sandbox::copyTree: that one applies the
// project-sandbox exclude list (build/, dist/, vendor/ …), which has nothing to
// do with a skill folder's helper files. We only drop .git — a clone's history
// is not part of the skill.
bool copyDir(const QString& src, const QString& dst) {
    QDir sd(src);
    if (!sd.exists()) return false;
    if (!QDir().mkpath(dst)) return false;
    const auto entries = sd.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot |
                                          QDir::Hidden);
    for (const QFileInfo& fi : entries) {
        const QString to = dst + QLatin1Char('/') + fi.fileName();
        if (fi.isDir()) {
            if (fi.fileName() == QLatin1String(".git")) continue;
            if (!copyDir(fi.absoluteFilePath(), to)) return false;
        } else {
            QFile::remove(to);  // QFile::copy refuses to overwrite
            if (!QFile::copy(fi.absoluteFilePath(), to)) return false;
        }
    }
    return true;
}

// Recursive delete, CONFINED to `guard`. Skills::remove()/install(--force) are
// the only callers, and both delete inside ~/.ollamadev/skills. A skill folder
// name is attacker-influenced, so we re-check containment on the CANONICAL path
// (after symlinks and ".." are resolved) rather than trusting how it was built.
// There is deliberately no shelling out to `rm -rf` anywhere in this file.
bool removeDirUnder(const QString& path, const QString& guard) {
    const QString canon = QFileInfo(path).canonicalFilePath();
    const QString canonGuard = QFileInfo(guard).canonicalFilePath();
    if (canon.isEmpty() || canonGuard.isEmpty()) return false;
    if (canon == canonGuard) return false;  // never wipe the skills root itself
    if (!canon.startsWith(canonGuard + QLatin1Char('/'))) return false;
    return QDir(canon).removeRecursively();
}

// A name safe to use as a single path segment.
QString sanitizeSegment(const QString& name) {
    static const QRegularExpression bad(QStringLiteral("[^a-zA-Z0-9._-]+"));
    QString s = name.trimmed();
    s.replace(bad, QStringLiteral("-"));
    while (s.startsWith('-') || s.startsWith('.')) s.remove(0, 1);
    while (s.endsWith('-') || s.endsWith('.')) s.chop(1);
    return s;
}

// Run an external tool with an ARGUMENT ARRAY. Never a composed shell string:
// `program` and every element of `args` reach execve() as separate argv entries,
// so a hostile source (";rm -rf ~", "$(…)", a path with spaces) is inert data.
bool runTool(const QString& program, const QStringList& args, QString* output = nullptr,
             int timeoutMs = 120000) {
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(program, args);
    if (!p.waitForStarted(10000)) {
        if (output) *output = QStringLiteral("could not run %1 (is it installed?)").arg(program);
        return false;
    }
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();  // OUR child, by handle — never a kill-by-name
        p.waitForFinished(2000);
        if (output) *output = QStringLiteral("%1 timed out").arg(program);
        return false;
    }
    if (output) *output = QString::fromUtf8(p.readAll()).trimmed();
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

// Qt does the HTTP, so there is no curl flag to get wrong and no shell involved.
bool download(const QString& url, const QString& dest) {
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(url)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QVariant::fromValue(QNetworkRequest::NoLessSafeRedirectPolicy));
    QScopedPointer<QNetworkReply> reply(nam.get(req));

    QEventLoop loop;
    QObject::connect(reply.data(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) return false;
    const QByteArray body = reply->readAll();
    if (body.isEmpty()) return false;
    QDir().mkpath(QFileInfo(dest).absolutePath());
    QFile f(dest);
    if (!f.open(QIODevice::WriteOnly)) return false;
    return f.write(body) == body.size();
}

}  // namespace

// ================================================================== Skills ===

QStringList Skills::baseDirs() {
    QStringList d{projectRoot() + QStringLiteral("/.ollamadev/skills"),
                  homeRoot() + QStringLiteral("/.ollamadev/skills")};
    d.removeDuplicates();
    return d;
}

QString Skills::homeDir() { return homeRoot() + QStringLiteral("/.ollamadev/skills"); }

QVector<Skill> Skills::all() { return allIn(projectRoot()); }

QString Skills::catalogFor(const QString& root) {
    QStringList lines;
    for (const Skill& s : allIn(root))
        lines << QStringLiteral("- %1: %2").arg(s.name, s.description);
    return lines.join('\n');
}

QVector<Skill> Skills::allIn(const QString& root) {
    QStringList bases{root + QStringLiteral("/.ollamadev/skills"),
                      homeRoot() + QStringLiteral("/.ollamadev/skills")};
    bases.removeDuplicates();

    QVector<Skill> out;
    QStringList seen;  // first writer wins: the project dir comes first
    for (const QString& base : bases) {
        QDir bd(base);
        if (!bd.exists()) continue;
        const auto dirs = bd.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString& sub : dirs) {
            const QString dir = base + QLatin1Char('/') + sub;
            const QString md = dir + QStringLiteral("/SKILL.md");
            if (!QFileInfo::exists(md)) continue;
            Skill s = parseSkillMd(md, sub);
            const QString key = s.name.toLower();
            if (seen.contains(key)) continue;
            seen << key;
            s.dir = dir;
            s.installed = true;
            out.append(s);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const Skill& a, const Skill& b) { return a.name.toLower() < b.name.toLower(); });
    return out;
}

QStringList Skills::names() {
    QStringList n;
    for (const Skill& s : all()) n << s.name;
    return n;
}

Skill Skills::get(const QString& name) {
    const QString want = name.trimmed();
    for (const Skill& s : all()) {
        if (s.name.compare(want, Qt::CaseInsensitive) != 0) continue;
        Skill full = s;
        full.body = parseSkillMd(s.dir + QStringLiteral("/SKILL.md"), s.name).body;
        const auto extras =
            QDir(s.dir).entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString& f : extras)
            if (f != QLatin1String("SKILL.md")) full.files << f;
        return full;
    }
    // Not on disk — fall back to the built-in library so the manager can show a
    // starter's real body before any crew run has materialised it.
    for (const SkillSpec& spec : CrewSkills::allBuiltins()) {
        if (spec.name.compare(want, Qt::CaseInsensitive) != 0) continue;
        Skill s;
        s.name = spec.name;
        s.description = spec.description;
        s.body = spec.body;
        s.builtin = true;
        return s;
    }
    return {};
}

QVector<Skill> Skills::builtins() {
    QStringList onDisk;
    for (const Skill& s : all()) onDisk << s.name.toLower();

    QVector<Skill> out;
    for (const SkillSpec& spec : CrewSkills::allBuiltins()) {
        if (onDisk.contains(spec.name.toLower())) continue;  // a user skill wins
        Skill s;
        s.name = spec.name;
        s.description = spec.description;
        s.builtin = true;
        out.append(s);
    }
    return out;
}

QVector<Skill> Skills::listForManager() {
    QVector<Skill> out = all();
    out += builtins();
    return out;
}

QString Skills::catalog() {
    QStringList lines;
    for (const Skill& s : all())
        lines << QStringLiteral("- %1: %2").arg(s.name, s.description);
    return lines.join('\n');
}

QString Skills::slugify(const QString& name) {
    const QString s = sanitizeSegment(name.toLower());
    return s.isEmpty() ? QStringLiteral("skill") : s;
}

// ---------------------------------------------------------------- registries

QString Skills::registryDir() { return homeRoot() + QStringLiteral("/.ollamadev/registry"); }

QStringList Skills::registries() {
    QStringList out{registryDir()};
    const QJsonValue v = Config::get(QStringLiteral("skills.registries"));
    if (v.isArray()) {
        const auto arr = v.toArray();
        for (const QJsonValue& e : arr) {
            const QString s = e.toString().trimmed();
            if (!s.isEmpty()) out << s;
        }
    }
    out.removeDuplicates();
    return out;
}

QVector<Skill> Skills::browse() {
    QStringList installed;
    for (const Skill& s : all()) installed << s.name.toLower();

    QVector<Skill> out;
    QStringList seen;
    for (const QString& src : registries()) {
        // A git/archive URL cannot be listed without fetching it, so remote
        // sources are install-only — browse shows what is actually browsable.
        if (!QFileInfo(src).isDir()) continue;
        for (const QString& dir : findSkillDirs(src)) {
            Skill s = parseSkillMd(dir + QStringLiteral("/SKILL.md"), QFileInfo(dir).fileName());
            const QString key = s.name.toLower();
            if (seen.contains(key)) continue;
            seen << key;
            s.dir = dir;
            s.source = src;
            s.installed = installed.contains(key);
            out.append(s);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const Skill& a, const Skill& b) { return a.name.toLower() < b.name.toLower(); });
    return out;
}

QVector<Skill> Skills::search(const QString& query) {
    const QString q = query.trimmed().toLower();

    QVector<Skill> pool = all();
    QStringList have;
    for (const Skill& s : pool) have << s.name.toLower();
    for (const Skill& s : browse())
        if (!have.contains(s.name.toLower())) pool.append(s);

    if (q.isEmpty()) {
        std::sort(pool.begin(), pool.end(), [](const Skill& a, const Skill& b) {
            return a.name.toLower() < b.name.toLower();
        });
        return pool;
    }
    QVector<Skill> hits;
    for (const Skill& s : pool)
        if (s.name.toLower().contains(q) || s.description.toLower().contains(q)) hits.append(s);
    return hits;
}

SkillInstall Skills::addFromRegistry(const QString& name, bool force) {
    for (const Skill& s : browse())
        if (s.name.compare(name, Qt::CaseInsensitive) == 0) return install(s.dir, force);
    return {{}, {QStringLiteral("not found in any registry: %1").arg(name)}};
}

// ------------------------------------------------------------------- install

SkillInstall Skills::install(const QString& source, bool force) {
    SkillInstall res;
    const QString src = source.trimmed();
    if (src.isEmpty()) {
        res.messages << QStringLiteral("no source given");
        return res;
    }

    // Auto-removed on scope exit — no `rm -rf` of a path we composed.
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        res.messages << QStringLiteral("could not create a staging directory");
        return res;
    }

    static const QRegularExpression archiveRx(QStringLiteral("\\.(tar\\.gz|tgz|zip)$"),
                                              QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression httpRx(QStringLiteral("^https?://"),
                                           QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression forgeRx(QStringLiteral("(github\\.com|gitlab\\.com|bitbucket\\.org)"),
                                            QRegularExpression::CaseInsensitiveOption);

    const bool isArchive = archiveRx.match(src).hasMatch();
    const bool isHttp = httpRx.match(src).hasMatch();
    const bool isGit = src.startsWith(QLatin1String("git@")) ||
                       src.endsWith(QLatin1String(".git")) ||
                       (isHttp && forgeRx.match(src).hasMatch() && !isArchive);

    QString scanBase;
    if (isGit) {
        // Flags verified against `git clone --help`: --depth <depth> (implies
        // --single-branch), --no-tags, -q/--quiet, and the `[--]` separator that
        // stops a source beginning with '-' from being read as an option.
        const QString repo = tmp.filePath(QStringLiteral("repo"));
        QString out;
        const bool ok = runTool(QStringLiteral("git"),
                                {QStringLiteral("clone"), QStringLiteral("--depth"),
                                 QStringLiteral("1"), QStringLiteral("--no-tags"),
                                 QStringLiteral("--quiet"), QStringLiteral("--"), src, repo},
                                &out, 300000);
        if (!ok || !QFileInfo(repo).isDir()) {
            res.messages << QStringLiteral("git clone failed: %1").arg(out);
            return res;
        }
        scanBase = repo;
    } else if (isArchive) {
        QString archive = src;
        if (isHttp) {
            const QString base = QFileInfo(QUrl(src).path()).fileName();
            archive = tmp.filePath(base.isEmpty() ? QStringLiteral("archive") : base);
            if (!download(src, archive)) {
                res.messages << QStringLiteral("download failed: %1").arg(src);
                return res;
            }
        }
        if (!QFileInfo(archive).isFile()) {
            res.messages << QStringLiteral("no such file: %1").arg(archive);
            return res;
        }
        const QString x = tmp.filePath(QStringLiteral("x"));
        QDir().mkpath(x);
        QString out;
        bool ok = false;
        if (archive.endsWith(QLatin1String(".zip"), Qt::CaseInsensitive)) {
            // `unzip -h`: -q quiet, -o overwrite without prompting, -d exdir.
            ok = runTool(QStringLiteral("unzip"),
                         {QStringLiteral("-q"), QStringLiteral("-o"), archive,
                          QStringLiteral("-d"), x},
                         &out);
        } else {
            // `tar --help`: -x extract, -z gzip, -f file, -C directory.
            ok = runTool(QStringLiteral("tar"),
                         {QStringLiteral("-xzf"), archive, QStringLiteral("-C"), x}, &out);
        }
        if (!ok) {
            res.messages << QStringLiteral("extract failed: %1").arg(out);
            return res;
        }
        scanBase = x;
    } else if (QFileInfo(src).isDir()) {
        scanBase = src;
    } else {
        res.messages << QStringLiteral(
                            "unrecognized source (expected a directory, a git URL, or a "
                            ".tar.gz/.zip): %1")
                            .arg(src);
        return res;
    }

    const QStringList dirs = findSkillDirs(scanBase);
    if (dirs.isEmpty()) {
        res.messages << QStringLiteral("no SKILL.md found under: %1").arg(src);
        return res;
    }

    const QString home = homeDir();
    QDir().mkpath(home);
    for (const QString& dir : dirs) {
        const Skill meta = parseSkillMd(dir + QStringLiteral("/SKILL.md"), QFileInfo(dir).fileName());
        const QString name = sanitizeSegment(meta.name);
        if (name.isEmpty()) {
            res.messages << QStringLiteral("skipped (bad name): %1").arg(dir);
            continue;
        }
        const QString dst = home + QLatin1Char('/') + name;
        if (QFileInfo(dst).isDir()) {
            if (!force) {
                res.messages << QStringLiteral("exists, skipped (use --force to overwrite): %1")
                                    .arg(name);
                continue;
            }
            removeDirUnder(dst, home);
        }
        if (copyDir(dir, dst)) res.installed << name;
        else res.messages << QStringLiteral("copy failed: %1").arg(name);
    }
    return res;
}

QString Skills::exportSkill(const QString& name, const QString& out) {
    const Skill s = get(name);
    if (s.isNull() || s.dir.isEmpty()) return {};  // a built-in has no folder to pack
    const QString base = QFileInfo(s.dir).fileName();
    const QString dest = out.isEmpty()
                             ? projectRoot() + QLatin1Char('/') + base + QStringLiteral(".skill.tar.gz")
                             : out;
    // `tar --help`: -c create, -z gzip, -f file, -C directory.
    QString log;
    runTool(QStringLiteral("tar"),
            {QStringLiteral("-czf"), dest, QStringLiteral("-C"), QFileInfo(s.dir).absolutePath(),
             base},
            &log);
    return QFileInfo(dest).isFile() ? dest : QString();
}

bool Skills::remove(const QString& name) {
    for (const Skill& s : all()) {
        if (s.name.compare(name, Qt::CaseInsensitive) != 0) continue;
        // Confined to the skills root the folder actually lives under, so a
        // crafted name can never escape into the rest of the filesystem.
        const QString root = QFileInfo(s.dir).absolutePath();
        return removeDirUnder(s.dir, root);
    }
    return false;
}

QString Skills::scaffold(const QString& name) {
    const QString slug = slugify(name);
    const QString md = homeDir() + QLatin1Char('/') + slug + QStringLiteral("/SKILL.md");
    if (!QFileInfo::exists(md)) {
        writeText(md, QStringLiteral("---\nname: %1\ndescription: One line on when to use this "
                                     "skill.\n---\n\n# %1\n\nStep-by-step instructions the model "
                                     "should follow when this skill is loaded.\n\n- You can "
                                     "reference helper files placed next to this SKILL.md.\n")
                          .arg(name));
    }
    return md;
}

QString Skills::save(const QString& name, const QString& description, const QString& body) {
    const QString n = name.trimmed();
    if (n.isEmpty()) return {};
    const QString slug = slugify(n);
    if (slug.isEmpty()) return {};

    static const QRegularExpression ws(QStringLiteral("\\s+"));
    QString desc = description.trimmed();
    desc.replace(ws, QStringLiteral(" "));
    if (desc.isEmpty()) desc = QStringLiteral("One line on when to use this skill.");

    QString b = body;
    while (b.startsWith('\n') || b.startsWith(' ')) b.remove(0, 1);

    const QString md = homeDir() + QLatin1Char('/') + slug + QStringLiteral("/SKILL.md");
    const QString text =
        QStringLiteral("---\nname: %1\ndescription: %2\n---\n\n%3\n").arg(n, desc, b);
    return writeText(md, text) ? slug : QString();
}

// ============================================================== CrewSkills ===

const QVector<SkillSpec>& CrewSkills::library() {
    static const QVector<SkillSpec> lib = {
        {QStringLiteral("responsive-design"),
         {"responsive", "website", "landing page", "web app", "pwa", "frontend", "spa", "dashboard"},
         QStringLiteral("Build layouts that work on phone, tablet, and desktop."),
         QStringLiteral(R"SK(# responsive-design

- Design mobile-first; add complexity at larger breakpoints, not the reverse.
- Use fluid units (%, rem, clamp(), min/max) and CSS grid/flex over fixed pixel widths.
- Test at ~360px, ~768px, and ~1280px. Nothing should overflow horizontally.
- Make tap targets >= 44px; never hide essential actions behind hover on touch.
- Use responsive images (srcset/sizes) and lazy-load below the fold.
)SK")},

        {QStringLiteral("semantic-html"),
         {"website", "landing page", "blog", "cms", "semantic markup", "docs site", "forum"},
         QStringLiteral("Use correct, accessible HTML structure."),
         QStringLiteral(R"SK(# semantic-html

- Use landmarks: <header>, <nav>, <main>, <article>, <footer> — one <main> per page.
- Exactly one <h1>; don't skip heading levels.
- Real <button>/<a> for actions/links — never a clickable <div>.
- Label every form control (<label for>) and associate errors with aria-describedby.
- Add alt text to meaningful images; alt="" for decorative ones.
)SK")},

        {QStringLiteral("seo-meta"),
         {"seo", "website", "landing page", "marketing", "blog", "cms", "docs"},
         QStringLiteral("Make pages discoverable and shareable."),
         QStringLiteral(R"SK(# seo-meta

- Unique <title> (<=60 chars) and <meta name=description> (<=155 chars) per page.
- Open Graph + Twitter card tags for link previews; a canonical <link>.
- Semantic headings, descriptive link text, and a sitemap.xml + robots.txt.
- Server-render or pre-render content that must be indexed.
- Add JSON-LD structured data where it fits (Article, Product, FAQ).
)SK")},

        {QStringLiteral("web-accessibility"),
         {"accessibility", "a11y", "website", "web app", "dashboard", "forms", "landing page"},
         QStringLiteral("Meet WCAG basics so everyone can use it."),
         QStringLiteral(R"SK(# web-accessibility

- Keyboard: every interactive element is reachable and operable with Tab/Enter/Space; visible focus ring.
- Color contrast >= 4.5:1 for text; never use color as the only signal.
- Use ARIA only to fill gaps native HTML can't — prefer native elements first.
- Respect prefers-reduced-motion; don't autoplay motion/sound.
- Test with a screen reader path for the primary flow.
)SK")},

        {QStringLiteral("pwa"),
         {"pwa", "progressive web", "offline", "service worker", "installable", "manifest"},
         QStringLiteral("Make a Progressive Web App installable and offline-capable."),
         QStringLiteral(R"SK(# pwa

- Ship a web app manifest (name, icons 192/512, start_url, display) and link it from the page.
- Register a service worker; precache the app shell, runtime-cache data with a clear strategy (stale-while-revalidate / network-first).
- Handle offline: a fallback page and graceful degradation when fetch fails.
- Version the cache and clean old caches on activate; don't cache POST/auth responses.
- Test install + offline in a fresh profile; serve over HTTPS (or localhost).
)SK")},

        {QStringLiteral("observability"),
         {"observability", "logging", "metrics", "tracing", "monitoring", "microservice",
          "serverless", "devops", "infra"},
         QStringLiteral("Make the system debuggable in production."),
         QStringLiteral(R"SK(# observability

- Structured logs (JSON) with a correlation/request id threaded through calls; never log secrets/PII.
- Emit metrics for the golden signals: latency, traffic, errors, saturation.
- Add health/readiness endpoints and meaningful startup/shutdown logs.
- Propagate trace context across service boundaries; record spans for slow paths.
- Make the log level configurable; fail loud on misconfig, not silently.
)SK")},

        {QStringLiteral("frontend-state"),
         {"web app", "spa", "react", "vue", "svelte", "angular", "state", "components"},
         QStringLiteral("Structure components and state predictably."),
         QStringLiteral(R"SK(# frontend-state

- Keep state minimal and derive the rest; lift shared state to the nearest common ancestor.
- Separate server cache (fetched data) from UI state; don't duplicate the source of truth.
- Handle loading / empty / error states explicitly for every async view.
- Memoize expensive renders; give list items stable keys (not the index).
- Clean up subscriptions/timers on unmount.
)SK")},

        {QStringLiteral("rest-api-design"),
         {"rest", "api", "backend", "microservice", "crud", "saas"},
         QStringLiteral("Design consistent, robust HTTP endpoints."),
         QStringLiteral(R"SK(# rest-api-design

- Noun resources, plural paths; HTTP verbs for actions (GET/POST/PUT/PATCH/DELETE).
- Correct status codes: 400 validation, 401 auth, 403 forbidden, 404 missing, 409 conflict, 422 semantic.
- Validate and sanitize every input at the boundary; never trust the client.
- Consistent error shape ({error, message, details}); paginate list endpoints.
- Be idempotent where the verb implies it; version the API.
)SK")},

        {QStringLiteral("graphql-schema"),
         {"graphql", "resolver", "schema"},
         QStringLiteral("Keep the schema clean and avoid N+1."),
         QStringLiteral(R"SK(# graphql-schema

- Design the schema around the client's needs; non-null where truly guaranteed.
- Batch/dataloader to kill N+1 resolver queries.
- Paginate connections (cursor-based) instead of returning unbounded lists.
- Enforce auth in resolvers/field guards, not just at the gateway.
- Limit query depth/complexity to prevent abuse.
)SK")},

        {QStringLiteral("auth-security"),
         {"auth", "login", "saas", "jwt", "session", "authn", "authz", "multi-tenant", "oauth"},
         QStringLiteral("Implement authentication and authorization safely."),
         QStringLiteral(R"SK(# auth-security

- Hash passwords with bcrypt/argon2 — never store or log plaintext.
- Authorize every request server-side; check the resource owner, not just "is logged in".
- In multi-tenant systems scope EVERY query by tenant id; deny by default.
- Short-lived access tokens + rotation; set HttpOnly + Secure + SameSite cookies.
- Rate-limit auth endpoints; protect state-changing requests from CSRF.
)SK")},

        {QStringLiteral("payments-money"),
         {"e-commerce", "ecommerce", "payment", "billing", "subscription", "checkout", "money",
          "tax", "saas", "orders", "cart"},
         QStringLiteral("Handle money, tax, and payments correctly."),
         QStringLiteral(R"SK(# payments-money

- Store money as integer minor units (cents) or decimal — NEVER float. Track currency explicitly.
- Compute totals/tax server-side from trusted prices; never trust client-sent amounts.
- Make charge/webhook handling idempotent (dedupe by event id) — webhooks retry.
- Verify payment provider webhook signatures; reconcile order state to provider state.
- Guard inventory against oversell with atomic decrements/transactions.
)SK")},

        {QStringLiteral("db-schema"),
         {"database", "schema", "migration", "sql", "query", "index", "postgres", "mysql"},
         QStringLiteral("Design schemas and write safe migrations."),
         QStringLiteral(R"SK(# db-schema

- Normalize first; denormalize only with a measured reason.
- Enforce integrity in the DB: foreign keys, NOT NULL, unique, check constraints.
- Index the columns you filter/join/sort on; watch for missing FK indexes.
- Migrations must be reversible and safe on live data — add columns nullable/with default, backfill, then constrain.
- Always parameterize queries; never string-concat user input.
)SK")},

        {QStringLiteral("etl-pipeline"),
         {"etl", "data pipeline", "idempot", "ingestion", "batch job"},
         QStringLiteral("Build pipelines that recover from partial failure."),
         QStringLiteral(R"SK(# etl-pipeline

- Make every stage idempotent so a re-run can't double-write.
- Validate the schema/shape of incoming data; quarantine bad records, don't crash the batch.
- Checkpoint progress so a failed run resumes instead of restarting.
- Make transforms deterministic and unit-testable on fixture data.
- Log row counts in/out per stage to catch silent drops.
)SK")},

        {QStringLiteral("realtime-ws"),
         {"realtime", "websocket", "sse", "socket", "reconnection", "rooms", "channels"},
         QStringLiteral("Get connection lifecycle and reconnection right."),
         QStringLiteral(R"SK(# realtime-ws

- Handle the full lifecycle: connect, heartbeat/ping, disconnect, reconnect with backoff.
- Authenticate on connect and re-check authz per message/room join.
- Apply backpressure — drop/coalesce when a client can't keep up.
- Make message handlers idempotent; clients may resend after reconnect.
- Clean up room membership and timers on disconnect to avoid leaks.
)SK")},

        {QStringLiteral("serverless"),
         {"serverless", "lambda", "cloud function", "workers", "cold start"},
         QStringLiteral("Write stateless, fast-starting functions."),
         QStringLiteral(R"SK(# serverless

- Stay stateless — persist to a store, never to local disk/memory between invocations.
- Minimize cold starts: lean dependencies, init reusable clients outside the handler.
- Read config/secrets from env or a secrets manager; least-privilege IAM per function.
- Set sane timeouts and handle partial/duplicate event delivery idempotently.
- Return structured errors; don't leak stack traces to callers.
)SK")},

        {QStringLiteral("cli-ux"),
         {"cli", "command-line", "command line", "args", "flags", "exit code", "terminal tool"},
         QStringLiteral("Make a command-line tool pleasant and scriptable."),
         QStringLiteral(R"SK(# cli-ux

- Provide --help and a clear usage line; support both short and long flags.
- Exit 0 on success, non-zero on failure; errors to stderr, data to stdout.
- Make output greppable; offer --json for machine consumption.
- Validate args early with actionable messages; never hang waiting silently.
- Be quiet by default, verbose with -v; confirm destructive actions unless --force.
)SK")},

        {QStringLiteral("library-api"),
         {"library", "sdk", "package", "public api", "semantic version"},
         QStringLiteral("Ship a clean, stable public API."),
         QStringLiteral(R"SK(# library-api

- Keep the public surface small and intentional; don't leak internals.
- Follow semantic versioning; treat any public change as a contract change.
- Document every exported symbol with an example.
- Fail loudly on misuse with clear errors; validate inputs at the boundary.
- No global mutable state or side effects on import.
)SK")},

        {QStringLiteral("testing-discipline"),
         {"test", "tests", "tdd", "qa", "unit test"},
         QStringLiteral("Write tests that actually catch regressions."),
         QStringLiteral(R"SK(# testing-discipline

- Test behavior and edge cases (empty, boundary, error), not just the happy path.
- One logical assertion per test; name tests by what they prove.
- Keep tests deterministic — no real network/clock/random; inject or mock them.
- Add a failing test that reproduces a bug BEFORE fixing it.
- Run the suite and confirm it passes before declaring done.
)SK")},

        {QStringLiteral("security-hardening"),
         {"security", "hardening", "injection", "secrets", "vulnerability", "csrf", "xss",
          "sql injection"},
         QStringLiteral("Eliminate the common vulnerability classes."),
         QStringLiteral(R"SK(# security-hardening

- Validate/escape all input; parameterize SQL; escape output to prevent XSS.
- Never commit secrets — use env/secret stores; scan the diff for keys/tokens before committing.
- Enforce authz on every sensitive action; deny by default.
- Avoid shelling out with user input; if unavoidable, use an arg array, never string interpolation.
- Keep dependencies updated; avoid known-vulnerable versions.
)SK")},

        {QStringLiteral("devops-iac"),
         {"devops", "infra", "docker", "ci/cd", "terraform", "iac", "deploy", "kubernetes"},
         QStringLiteral("Write safe, idempotent infrastructure & pipelines."),
         QStringLiteral(R"SK(# devops-iac

- Make everything idempotent and declarative; the same apply twice = no change.
- Never hard-code secrets — inject from a secret store; least-privilege everywhere.
- Pin versions/images; small, cache-friendly, fail-fast pipeline stages.
- Make changes reversible (plan/diff before apply); guard production behind review.
- Add health checks and surface logs/metrics.
)SK")},

        {QStringLiteral("llm-app"),
         {"llm", "ai app", "prompt", "agent loop", "token", "streaming", "embedding", "rag"},
         QStringLiteral("Build reliable LLM-powered features."),
         QStringLiteral(R"SK(# llm-app

- Budget tokens explicitly; truncate/summarize context before you hit the limit.
- Stream responses for UX; handle partial output and mid-stream errors.
- Validate/parse model output defensively — assume it can be malformed (use schema/JSON mode).
- Cache deterministic calls; add retries with backoff for transient failures.
- Never trust model output in privileged actions without a guard/confirmation.
)SK")},

        {QStringLiteral("smart-contract"),
         {"solidity", "smart contract", "web3", "dapp", "reentrancy", "gas"},
         QStringLiteral("Write secure on-chain code."),
         QStringLiteral(R"SK(# smart-contract

- Checks-effects-interactions order; guard against reentrancy.
- Use safe math / a vetted library; watch for overflow and rounding.
- Minimize and audit external calls; never trust their return blindly.
- Restrict privileged functions with access control; emit events for state changes.
- Write exhaustive tests including adversarial cases before deploy.
)SK")},

        {QStringLiteral("game-loop"),
         {"game", "unity", "godot", "phaser", "game loop"},
         QStringLiteral("Keep the game loop smooth and inputs responsive."),
         QStringLiteral(R"SK(# game-loop

- Separate update (fixed timestep) from render; scale movement by delta time.
- Pool/reuse objects instead of allocating each frame; avoid GC spikes.
- Keep per-frame work bounded; profile before optimizing.
- Handle input in the loop, not via blocking calls.
- Load assets async; never stall the loop on I/O.
)SK")},

        {QStringLiteral("data-ml"),
         {"ml", "data/ml", "pandas", "numpy", "torch", "scikit", "notebook", "machine learning"},
         QStringLiteral("Make data/ML work reproducible."),
         QStringLiteral(R"SK(# data-ml

- Set and record random seeds; pin library versions for reproducibility.
- Validate data (shapes, nulls, ranges) before training; never leak test into train.
- Keep preprocessing in code, not manual notebook steps; make scripts re-runnable.
- Track metrics and the params that produced them.
- Separate data loading, transform, model, and eval into testable pieces.
)SK")},

        {QStringLiteral("mobile-app"),
         {"mobile app", "ios", "android", "react native", "flutter"},
         QStringLiteral("Respect platform lifecycle and UX guidelines."),
         QStringLiteral(R"SK(# mobile-app

- Handle the app lifecycle (background/foreground, low memory) and save state.
- Keep the main/UI thread free; do I/O and heavy work off it.
- Follow each platform's navigation and UX conventions.
- Handle offline and flaky networks gracefully; cache and retry.
- Request permissions just-in-time with clear rationale.
)SK")},

        {QStringLiteral("desktop-app"),
         {"desktop app", "electron", "tauri", "qt", "gtk"},
         QStringLiteral("Mind windowing, packaging, and OS integration."),
         QStringLiteral(R"SK(# desktop-app

- Keep heavy work off the UI thread/process; keep the window responsive.
- Sandbox/limit privileges of any web/renderer layer; validate IPC messages.
- Handle multi-window, focus, and OS lifecycle (sleep/quit) cleanly.
- Persist user data in the OS-appropriate location; clean up child processes on exit.
- Test packaging on the target OS.
)SK")},

        {QStringLiteral("browser-extension"),
         {"browser extension", "chrome extension", "manifest v3", "content script",
          "background script"},
         QStringLiteral("Follow Manifest V3 and least privilege."),
         QStringLiteral(R"SK(# browser-extension

- Request the minimum permissions and host_permissions; justify each.
- Keep the service worker lean and event-driven (MV3 has no persistent background).
- Validate messages between content/background scripts; never trust page content.
- Don't inject more into pages than needed; clean up on disable.
- Avoid remote code; bundle everything.
)SK")},

        {QStringLiteral("bot-platform"),
         {"bot", "discord", "slack", "telegram"},
         QStringLiteral("Build a chat bot that respects platform limits."),
         QStringLiteral(R"SK(# bot-platform

- Keep the platform token secret (env, not code); rotate if leaked.
- Respect rate limits — queue/backoff instead of hammering the API.
- Acknowledge events fast; do slow work async to avoid timeouts.
- Validate and authorize commands; sanitize anything echoed back.
- Handle reconnects and missed events idempotently.
)SK")},

        {QStringLiteral("embedded"),
         {"embedded", "iot", "firmware", "microcontroller", "interrupt", "micropython"},
         QStringLiteral("Respect memory, timing, and hardware constraints."),
         QStringLiteral(R"SK(# embedded

- Avoid dynamic allocation in hot/interrupt paths; bound all buffers.
- Keep ISRs tiny — set a flag, defer work to the main loop.
- Mind timing and watchdogs; don't block on I/O.
- Validate hardware register access; handle peripheral failure.
- Be explicit about integer widths and endianness.
)SK")},

        {QStringLiteral("refactor-safety"),
         {"refactor", "restructure", "clean up", "tech debt"},
         QStringLiteral("Change structure without changing behavior."),
         QStringLiteral(R"SK(# refactor-safety

- Ensure tests cover the behavior BEFORE refactoring; add them if missing.
- Make small, reversible steps; keep it green between each.
- Don't mix refactor + behavior change in one commit.
- Preserve the public interface unless the task is to change it.
- Re-run the suite after each step to prove behavior is unchanged.
)SK")},

        {QStringLiteral("docs-writing"),
         {"docs", "documentation", "readme", "docusaurus", "mkdocs", "docs site"},
         QStringLiteral("Write docs people can actually follow."),
         QStringLiteral(R"SK(# docs-writing

- Lead with what it does and a copy-paste quickstart that works.
- Show runnable examples; keep them tested/accurate.
- Document the why and the gotchas, not just the API surface.
- Use clear headings and consistent terminology; link related pages.
- Keep docs next to the code and update them with the change.
)SK")},
    };
    return lib;
}

const QVector<SkillSpec>& CrewSkills::teamLibrary() {
    // slug, description, triggers, bullets — the body is assembled from the
    // bullets so a team starter always reads the same way.
    struct Team {
        const char* slug;
        const char* desc;
        QStringList triggers;
        QStringList bullets;
    };
    static const QVector<SkillSpec> teams = [] {
        const QVector<Team> raw = {
            {"website", "Website (static / marketing / content) — project starter.",
             {"website", "marketing site"},
             {"Semantic, accessible markup — landmarks, one <h1>, real buttons/links.",
              "Responsive, mobile-first; nothing overflows on small screens.",
              "SEO basics: unique title/description, Open Graph, sitemap.",
              "Fast load: optimize and lazy-load images, keep JS minimal."}},
            {"landing-page", "High-converting landing page — project starter.",
             {"landing page"},
             {"One clear hero and call-to-action above the fold.",
              "Responsive and fast; remove distractions from the goal.",
              "SEO/meta and Open Graph for shareable previews.",
              "A working contact/signup form with validation and feedback."}},
            {"web-app", "Web application (SPA or full-stack) — project starter.",
             {"web app", "single-page app"},
             {"Clear component structure with predictable, minimal state.",
              "Routing and auth wired; protect private routes.",
              "Handle loading / empty / error states for every async view.",
              "Integrate APIs defensively; add tests for core flows."}},
            {"saas", "SaaS product (multi-tenant) — project starter.",
             {"saas"},
             {"Scope EVERY query by tenant id; deny by default.",
              "Secure auth and sessions; least-privilege roles.",
              "Reliable billing/subscription logic with idempotent webhooks.",
              "Per-tenant limits and an audit log for sensitive actions."}},
            {"ecommerce", "E-commerce (catalog / cart / checkout) — project starter.",
             {"e-commerce", "ecommerce"},
             {"Store money as integer minor units, never float; track currency.",
              "Compute totals and tax server-side from trusted prices.",
              "Make payment and webhook handling idempotent; verify signatures.",
              "Guard inventory against oversell with atomic updates."}},
            {"admin-dashboard", "Admin dashboard / internal tool — project starter.",
             {"admin dashboard", "internal tool"},
             {"Accurate data with server-side pagination, sort, and filter.",
              "Role-based access checked on every action.",
              "Clear tables, forms, and charts; confirm destructive operations.",
              "Optimistic UI with rollback on failure."}},
            {"blog-cms", "Blog or CMS — project starter.",
             {"blog", "cms"},
             {"Clear content models and a stable slug/permalink scheme.",
              "Sanitize and safely render user/markdown content.",
              "SEO and RSS; draft/publish workflow.",
              "Cache rendered content; invalidate on edit."}},
            {"docs-site", "Documentation site — project starter.",
             {"docs site", "documentation site"},
             {"Clear navigation and working search.",
              "Runnable, tested code samples; keep them accurate.",
              "Logical structure and consistent terminology.",
              "Versioned docs and a fast static build."}},
            {"forum-community", "Forum / community app — project starter.",
             {"forum", "community"},
             {"Data integrity for threads, posts, and users.",
              "Moderation tools plus spam/abuse handling.",
              "Rate-limit posting; paginate and index for scale.",
              "Notifications without N+1 query blowups."}},
            {"pwa-app", "Progressive Web App — project starter.",
             {"progressive web app"},
             {"Ship a manifest; make it installable.",
              "Service worker: precache the shell, runtime-cache data.",
              "Offline fallback and graceful degradation.",
              "Version caches and clean old ones; serve over HTTPS."}},
            {"mobile", "Mobile app (iOS / Android / RN / Flutter) — project starter.",
             {"mobile app"},
             {"Handle the app lifecycle and restore state.",
              "Keep the UI thread free; do heavy work off it.",
              "Follow each platform navigation and UX conventions.",
              "Handle offline and flaky networks; request permissions just-in-time."}},
            {"desktop", "Desktop app (Electron / Tauri / Qt / GTK) — project starter.",
             {"desktop app"},
             {"Keep heavy work off the UI thread; stay responsive.",
              "Sandbox any renderer and validate IPC messages.",
              "Handle multi-window and OS lifecycle cleanly.",
              "Store user data in OS-appropriate paths; test packaging."}},
            {"rest-api", "REST API / backend service — project starter.",
             {"rest api", "backend service"},
             {"Noun resources, correct verbs and status codes.",
              "Validate every input at the boundary.",
              "Consistent error shape; paginate list endpoints.",
              "Authorize per request; add tests."}},
            {"graphql", "GraphQL API — project starter.",
             {"graphql api"},
             {"Design the schema around client needs.",
              "Use dataloader/batching to kill N+1.",
              "Cursor-based pagination instead of unbounded lists.",
              "Enforce auth in resolvers; limit query depth/complexity."}},
            {"realtime", "Realtime / WebSocket service — project starter.",
             {"realtime service", "websocket service"},
             {"Handle the full connection lifecycle with reconnect backoff.",
              "Authenticate on connect and per room/channel join.",
              "Apply backpressure when a client cannot keep up.",
              "Make handlers idempotent; clean up on disconnect."}},
            {"serverless-fn", "Serverless functions — project starter.",
             {"serverless function", "cloud function"},
             {"Stay stateless; never persist to local disk/memory.",
              "Minimize cold starts; init clients outside the handler.",
              "Secrets from env/manager; least-privilege IAM per function.",
              "Idempotent on duplicate events; return structured errors."}},
            {"microservice", "Microservice — project starter.",
             {"microservice"},
             {"Single responsibility and a clear API contract.",
              "Health/readiness checks and meaningful logs.",
              "Observability: logs, metrics, traces with a correlation id.",
              "Resilient calls: timeouts, retries, circuit breaking."}},
            {"database", "Database / schema work — project starter.",
             {"database schema", "migrations"},
             {"Normalize first; index what you filter, join, and sort on.",
              "Enforce integrity: foreign keys, NOT NULL, unique, checks.",
              "Reversible, safe-on-live migrations: add nullable, backfill, constrain.",
              "Parameterize queries; back up before destructive changes."}},
            {"data-pipeline", "Data pipeline / ETL — project starter.",
             {"data pipeline", "etl job"},
             {"Make every stage idempotent so a re-run cannot double-write.",
              "Validate incoming data; quarantine bad records.",
              "Checkpoint progress so a failed run resumes.",
              "Deterministic, testable transforms; log row counts in/out."}},
            {"data-ml-project", "Data / ML project — project starter.",
             {"data science project", "machine learning project"},
             {"Set and record seeds; pin library versions.",
              "Validate data; never leak test into train.",
              "Keep preprocessing in re-runnable code, not manual steps.",
              "Track metrics and the params that produced them."}},
            {"ai-app", "AI / LLM app — project starter.",
             {"ai app", "llm application"},
             {"Budget tokens; truncate or summarize before the limit.",
              "Stream output; handle partial and mid-stream errors.",
              "Parse model output defensively (schema/JSON mode).",
              "Cache deterministic calls; guard privileged actions."}},
            {"game", "Game — project starter.",
             {"game project"},
             {"Separate fixed-timestep update from render; scale by delta time.",
              "Pool/reuse objects to avoid per-frame GC spikes.",
              "Keep per-frame work bounded; profile before optimizing.",
              "Load assets async; never stall the loop on I/O."}},
            {"cli", "CLI tool — project starter.",
             {"cli tool", "command-line tool"},
             {"Provide --help and a clear usage line.",
              "Exit 0 on success; errors to stderr, data to stdout.",
              "Offer --json for machine consumption.",
              "Validate args early; confirm destructive actions unless --force."}},
            {"library", "Library / SDK / package — project starter.",
             {"library", "sdk package"},
             {"Keep the public surface small and intentional.",
              "Follow semantic versioning; document every export with an example.",
              "Fail loudly on misuse; validate inputs at the boundary.",
              "No global mutable state or side effects on import."}},
            {"browser-ext", "Browser extension (Manifest V3) — project starter.",
             {"browser extension", "chrome extension"},
             {"Request the minimum permissions; justify each.",
              "Keep the service worker lean and event-driven.",
              "Validate messages between content and background scripts.",
              "Bundle everything; no remote code."}},
            {"vscode-ext", "VS Code extension — project starter.",
             {"vs code extension", "vscode extension"},
             {"Declare contribution points and activation events; keep activation lean.",
              "Use the extension API and dispose resources you create.",
              "Handle workspace and multi-root cases.",
              "Test in the Extension Development Host."}},
            {"plugin", "Plugin (WordPress / Figma / Obsidian / …) — project starter.",
             {"plugin for"},
             {"Follow the host plugin API, hooks, and lifecycle.",
              "Do not pollute global or host state.",
              "Validate inputs coming from the host.",
              "Clean packaging and versioning; fail gracefully on host API changes."}},
            {"chatbot", "Chat bot (Discord / Slack / Telegram) — project starter.",
             {"chat bot"},
             {"Keep the platform token secret (env, not code).",
              "Respect rate limits; queue and back off.",
              "Acknowledge events fast; do slow work async.",
              "Validate and authorize commands; stay idempotent on reconnect."}},
            {"automation", "Automation / script — project starter.",
             {"automation script"},
             {"Robust I/O and error handling.",
              "Idempotent so it is safe to re-run.",
              "Structured logging; clear exit codes.",
              "Handle credentials safely; never hard-code secrets."}},
            {"devops", "DevOps / infra — project starter.",
             {"devops", "infrastructure"},
             {"Make everything idempotent and declarative.",
              "Secrets from a store; least privilege everywhere.",
              "Pin versions/images; fail-fast, cache-friendly stages.",
              "Plan/diff before apply; keep changes reversible."}},
            {"ci-cd", "CI/CD pipeline — project starter.",
             {"ci/cd pipeline", "ci cd"},
             {"Stage build, test, then deploy.",
              "Cache dependencies; fail fast.",
              "Secrets via the CI store; never print them to logs.",
              "Reproducible and pinned; gate production behind review."}},
            {"embedded-iot", "Embedded / IoT / firmware — project starter.",
             {"embedded", "firmware", "iot device"},
             {"Avoid dynamic allocation in hot/interrupt paths; bound buffers.",
              "Keep ISRs tiny; defer work to the main loop.",
              "Mind timing and watchdogs; do not block on I/O.",
              "Be explicit about integer widths and endianness."}},
            {"web3", "Smart contract / web3 dApp — project starter.",
             {"smart contract", "web3 dapp"},
             {"Checks-effects-interactions order; guard against reentrancy.",
              "Use safe math; watch overflow and rounding.",
              "Minimize and audit external calls; emit events on state change.",
              "Write adversarial tests before deploy."}},
            {"security-project", "Security hardening project — project starter.",
             {"security hardening project"},
             {"Validate/escape input; parameterize SQL; escape output.",
              "Never commit secrets; scan the diff before committing.",
              "Authorize every sensitive action; deny by default.",
              "Avoid shelling out with user input; keep dependencies patched."}},
        };
        QVector<SkillSpec> out;
        out.reserve(raw.size());
        for (const Team& t : raw) {
            SkillSpec s;
            s.name = QString::fromUtf8(t.slug);
            s.description = QString::fromUtf8(t.desc);
            s.triggers = t.triggers;
            s.body = QStringLiteral("# %1\n\n- %2\n")
                         .arg(s.name, t.bullets.join(QStringLiteral("\n- ")));
            out.append(s);
        }
        return out;
    }();
    return teams;
}

QVector<SkillSpec> CrewSkills::allBuiltins() {
    QVector<SkillSpec> out = library();
    out += teamLibrary();
    return out;
}

QVector<SkillSpec> CrewSkills::byNames(const QStringList& names) {
    const QVector<SkillSpec> lib = allBuiltins();
    QVector<SkillSpec> out;
    QStringList seen;
    for (const QString& raw : names) {
        const QString n = raw.trimmed().toLower();
        if (n.isEmpty() || seen.contains(n)) continue;
        for (const SkillSpec& s : lib) {
            if (s.name.toLower() != n) continue;
            seen << n;
            out.append(s);
            break;
        }
    }
    return out;
}

QVector<SkillSpec> CrewSkills::forFocus(const QString& focus, int cap) {
    const QString f = focus.trimmed().toLower();
    if (f.isEmpty() || cap <= 0) return {};

    // Score = the LONGEST trigger that occurs in the focus text. A long trigger
    // ("progressive web app") is a far stronger signal than a short one ("api"),
    // so the most specific starters survive the cap.
    QVector<QPair<int, SkillSpec>> hits;
    for (const SkillSpec& s : library()) {
        int best = 0;
        for (const QString& t : s.triggers)
            if (f.contains(t.toLower())) best = std::max(best, static_cast<int>(t.size()));
        if (best > 0) hits.append({best, s});
    }
    std::stable_sort(hits.begin(), hits.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });

    QVector<SkillSpec> out;
    for (const auto& h : hits) {
        if (out.size() >= cap) break;
        out.append(h.second);
    }
    return out;
}

QVector<SkillSpec> CrewSkills::resolve(const QString& focus, const QStringList& force, int cap) {
    QVector<SkillSpec> out = byNames(force);  // forced first — never dropped by the cap
    QStringList have;
    for (const SkillSpec& s : out) have << s.name;
    for (const SkillSpec& s : forFocus(focus, cap))
        if (!have.contains(s.name)) out.append(s);
    return out;
}

QStringList CrewSkills::materialize(const QVector<SkillSpec>& skills, const QString& baseDir) {
    const QString home = homeRoot();
    QStringList present;
    for (const SkillSpec& s : skills) {
        if (s.name.isEmpty()) continue;
        // A user's own skill of this name wins — never clobber a customised one.
        const QString mine =
            home + QStringLiteral("/.ollamadev/skills/") + s.name + QStringLiteral("/SKILL.md");
        if (QFileInfo::exists(mine)) {
            present << s.name;
            continue;
        }
        const QString md = baseDir + QStringLiteral("/.ollamadev/skills/") + s.name +
                           QStringLiteral("/SKILL.md");
        if (QFileInfo::exists(md)) {
            present << s.name;
            continue;
        }
        const QString text = QStringLiteral("---\nname: %1\ndescription: %2\n---\n\n%3")
                                 .arg(s.name, s.description, s.body);
        if (writeText(md, text)) present << s.name;
    }
    return present;
}

// =============================================================== CrewRoles ===

namespace {

// 'coder' is the default and the fallback: it must always exist.
const QVector<CrewRole>& builtinRoles() {
    static const QVector<CrewRole> roles = {
        {QStringLiteral("coder"),
         QStringLiteral("General implementation — write or modify code to satisfy the subtask."),
         QStringLiteral("You are a coding agent in an isolated sandbox copy of the project. You "
                        "MUST actually make the changes by calling your tools (write/edit/bash) — "
                        "do not merely describe them. Keep changes focused; when the files are "
                        "written, stop."),
         {}, false, false},
        {QStringLiteral("tester"),
         QStringLiteral("Write or extend automated tests for the subtask."),
         QStringLiteral("You are a test-writing agent in an isolated sandbox copy of the project. "
                        "Add or extend AUTOMATED TESTS only — match the project's existing test "
                        "framework, layout, and naming conventions. Do not change production code "
                        "beyond trivial test hooks. Actually write the test files with your tools; "
                        "when the tests are in place, stop."),
         {}, false, false},
        {QStringLiteral("docs"),
         QStringLiteral("Update documentation, README, comments, or changelog for the subtask."),
         QStringLiteral("You are a documentation agent in an isolated sandbox copy of the project. "
                        "Update docs, README, code comments, or the changelog to match the change — "
                        "clear, concise, and matching the project's tone. Do not alter program "
                        "logic. Write the files with your tools; when done, stop."),
         {}, false, false},
        {QStringLiteral("refactor"),
         QStringLiteral("Restructure code for clarity/efficiency WITHOUT changing behavior."),
         QStringLiteral("You are a refactoring agent in an isolated sandbox copy of the project. "
                        "Improve structure, naming, and clarity WITHOUT changing observable "
                        "behavior or public interfaces, and without adding features. Make the "
                        "edits with your tools; keep the diff focused; when done, stop."),
         {}, false, false},
        {QStringLiteral("security"),
         QStringLiteral("Harden the code: injection, unsafe shell, secrets, weak validation."),
         QStringLiteral("You are a security-hardening agent in an isolated sandbox copy of the "
                        "project. Fix concrete vulnerabilities relevant to this subtask — "
                        "injection, unsafe shell/eval, path traversal, weak input validation, "
                        "leaked secrets — without breaking behavior. Make the edits with your "
                        "tools; when done, stop."),
         {}, false, false},
        {QStringLiteral("reviewer"),
         QStringLiteral("Survey and advise only — never edits (read-only)."),
         QStringLiteral("You are a review agent in an isolated sandbox copy of the project. You are "
                        "READ-ONLY: investigate with ls/grep/view/glob and report findings. Do NOT "
                        "write, edit, or delete anything — your changeset must stay empty. When you "
                        "have reported, stop."),
         {}, true, false},
    };
    return roles;
}

}  // namespace

QString CrewRoles::dir() {
    const QString d = homeRoot() + QStringLiteral("/.ollamadev/crew-roles");
    QDir().mkpath(d);
    return d;
}

QString CrewRoles::normName(const QString& name) {
    const QString n = sanitizeSegment(name.toLower());
    return n.isEmpty() ? QStringLiteral("role") : n;
}

QVector<CrewRole> CrewRoles::all() {
    QVector<CrewRole> roles = builtinRoles();

    const auto files = QDir(dir()).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString& f : files) {
        const QString path = dir() + QLatin1Char('/') + f;
        const QJsonObject j = json::objectFrom(readText(path));
        if (j.isEmpty()) continue;

        const QString name = normName(j.value(QStringLiteral("name"))
                                          .toString(QFileInfo(f).completeBaseName()));
        // A user file may override a built-in by reusing its name; fields it omits
        // fall back to the built-in's, so overriding just the model is one key.
        int at = -1;
        for (int i = 0; i < roles.size(); ++i)
            if (roles[i].name == name) at = i;

        CrewRole r = at >= 0 ? roles[at] : CrewRole{};
        r.name = name;
        if (j.contains(QStringLiteral("desc")))
            r.desc = j.value(QStringLiteral("desc")).toString().trimmed();
        if (j.contains(QStringLiteral("prompt")))
            r.prompt = j.value(QStringLiteral("prompt")).toString().trimmed();
        r.model = j.value(QStringLiteral("model")).toString().trimmed();
        r.readOnly = j.value(QStringLiteral("permission")).toString() == QLatin1String("readonly");
        r.custom = true;

        if (at >= 0) roles[at] = r;
        else roles.append(r);
    }
    return roles;
}

QStringList CrewRoles::names() {
    QStringList n;
    for (const CrewRole& r : all()) n << r.name;
    return n;
}

CrewRole CrewRoles::get(const QString& name) {
    const QVector<CrewRole> roles = all();
    const QString n = normName(name);
    for (const CrewRole& r : roles)
        if (r.name == n) return r;
    // A Director that invents a role must not strand the subtask.
    for (const CrewRole& r : roles)
        if (r.name == QLatin1String("coder")) return r;
    return roles.isEmpty() ? CrewRole{} : roles.first();
}

QString CrewRoles::add(const QString& name, const QString& prompt, const QString& desc,
                       const QString& model, bool readOnly) {
    const QString n = normName(name);
    const QJsonObject def{
        {"name", n},
        {"desc", desc.trimmed()},
        {"prompt", prompt.trimmed()},
        {"model", model.trimmed()},
        {"permission", readOnly ? QStringLiteral("readonly") : QStringLiteral("auto")}};
    const QString path = dir() + QLatin1Char('/') + n + QStringLiteral(".json");
    return writeText(path, QString::fromUtf8(QJsonDocument(def).toJson(QJsonDocument::Indented)))
               ? path
               : QString();
}

bool CrewRoles::remove(const QString& name) {
    const QString path = dir() + QLatin1Char('/') + normName(name) + QStringLiteral(".json");
    if (!QFileInfo(path).isFile()) return false;  // a built-in has no file
    return QFile::remove(path);
}

QString CrewRoles::catalog() {
    QStringList lines;
    for (const CrewRole& r : all())
        lines << QStringLiteral("- %1: %2").arg(
            r.name, r.desc.isEmpty() ? QStringLiteral("no description") : r.desc);
    return lines.join('\n');
}

QString CrewRoles::persona(const QString& name) {
    const CrewRole r = get(name);
    if (r.prompt.isEmpty()) return {};
    QString s = QStringLiteral("\n\nROLE — %1: %2").arg(r.name, r.prompt);
    // A read-only role is enforced by INSTRUCTION, not by Permission::setMode():
    // the permission mode is process-global, and crew coders run concurrently, so
    // flipping it here would put every other coder in the run into read-only too.
    // The empty changeset it produces is the real backstop.
    if (r.readOnly)
        s += QStringLiteral(
            " This role is READ-ONLY: do not create, edit, or delete any file — report your "
            "findings in your final message instead.");
    return s;
}

// =============================================================== CrewPacks ===

QString CrewPacks::dir() {
    const QString d = homeRoot() + QStringLiteral("/.ollamadev/crew-packs");
    QDir().mkpath(d);
    return d;
}

QStringList CrewPacks::keys() {
    // The REUSABLE knobs of a team. Deliberately no task/runId: a pack is a team,
    // not a job.
    return {QStringLiteral("focus"),           QStringLiteral("directorModel"),
            QStringLiteral("coderModel"),      QStringLiteral("auditorModel"),
            QStringLiteral("researcherModel"), QStringLiteral("directorBackend"),
            QStringLiteral("coderBackend"),    QStringLiteral("auditorBackend"),
            QStringLiteral("researcherBackend"), QStringLiteral("max"),
            QStringLiteral("amplify"),         QStringLiteral("land"),
            QStringLiteral("research"),        QStringLiteral("audit"),
            QStringLiteral("skills"),          QStringLiteral("hosts")};
}

QJsonObject CrewPacks::builtins() {
    // focus is what actually steers the Director's plan; amplify adds an
    // adversarial reviewer panel where it pays for itself.
    return QJsonObject{
        {"web-app",
         QJsonObject{{"focus", "a web application — an HTML/CSS/JS frontend plus its backend; "
                               "prioritise a working UI, sensible routing, and a clean separation "
                               "of concerns"}}},
        {"rest-api",
         QJsonObject{{"focus", "a REST API — clear resource endpoints, input validation, "
                               "consistent error responses, and a test for each route"}}},
        {"cli-tool",
         QJsonObject{{"focus", "a command-line tool — argument parsing, a helpful --help, clear "
                               "error messages, and correct exit codes"}}},
        {"data-pipeline",
         QJsonObject{{"focus", "a data-processing pipeline — robust parsing, transformation, "
                               "validation, and explicit handling of malformed or edge-case "
                               "input"}}},
        {"library",
         QJsonObject{{"focus", "a reusable library/package — a small clear public API, docblocks, "
                               "no side effects on import, and unit tests"}}},
        {"bugfix",
         QJsonObject{{"focus", "find and fix the bug with the smallest correct change, then add a "
                               "regression test that fails before the fix and passes after"},
                     {"amplify", 3}}},
        {"refactor",
         QJsonObject{{"focus", "refactor for clarity and structure WITHOUT changing behaviour; "
                               "keep the public API stable and the diff reviewable"},
                     {"amplify", 3}}},
        {"tested",
         QJsonObject{{"focus", "build with test-first discipline — every change covered by a test "
                               "that runs green; do not finish with failing or missing tests"}}},
    };
}

QString CrewPacks::save(const QString& name, const QJsonObject& opts) {
    QJsonObject pack;
    for (const QString& k : keys())
        if (opts.contains(k)) pack.insert(k, opts.value(k));
    const QString path =
        dir() + QLatin1Char('/') + sanitizeSegment(name) + QStringLiteral(".json");
    return writeText(path, QString::fromUtf8(QJsonDocument(pack).toJson(QJsonDocument::Indented)))
               ? path
               : QString();
}

QJsonObject CrewPacks::load(const QString& name) {
    QJsonObject src;
    const QString path =
        dir() + QLatin1Char('/') + sanitizeSegment(name) + QStringLiteral(".json");
    if (QFileInfo(path).isFile()) src = json::objectFrom(readText(path));
    // A user pack wins over a built-in of the same name.
    if (src.isEmpty()) src = builtins().value(name).toObject();
    if (src.isEmpty()) return {};

    QJsonObject out;
    for (const QString& k : keys())
        if (src.contains(k)) out.insert(k, src.value(k));
    return out;
}

bool CrewPacks::exists(const QString& name) { return !load(name).isEmpty(); }

namespace {

// One-line summary of a pack's reusable knobs, for `crew pack list`.
QString packSummary(const QJsonObject& j) {
    QStringList bits;
    const QString focus = j.value(QStringLiteral("focus")).toString();
    if (!focus.isEmpty())
        bits << QStringLiteral("focus: %1")
                    .arg(focus.size() > 60 ? focus.left(57) + QStringLiteral("…") : focus);
    const QString cb = j.value(QStringLiteral("coderBackend")).toString();
    if (!cb.isEmpty()) bits << QStringLiteral("coder backend: %1").arg(cb);
    const QString cm = j.value(QStringLiteral("coderModel")).toString();
    if (!cm.isEmpty()) bits << QStringLiteral("coder: %1").arg(cm);
    const int amp = j.value(QStringLiteral("amplify")).toInt();
    if (amp > 1) bits << QStringLiteral("amplify ×%1").arg(amp);
    const int max = j.value(QStringLiteral("max")).toInt();
    if (max > 0) bits << QStringLiteral("%1 coders").arg(max);
    return bits.isEmpty() ? QStringLiteral("(empty pack)") : bits.join(QStringLiteral(" · "));
}

}  // namespace

QVector<QPair<QString, QString>> CrewPacks::all() {
    QMap<QString, QString> out;  // sorted by name
    const QJsonObject b = builtins();
    for (auto it = b.constBegin(); it != b.constEnd(); ++it)
        out.insert(it.key(), packSummary(it.value().toObject()) + QStringLiteral("  (built-in)"));

    const auto files = QDir(dir()).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString& f : files) {
        const QJsonObject j = json::objectFrom(readText(dir() + QLatin1Char('/') + f));
        if (j.isEmpty()) continue;
        out.insert(QFileInfo(f).completeBaseName(), packSummary(j));  // a user pack overrides
    }

    QVector<QPair<QString, QString>> v;
    for (auto it = out.constBegin(); it != out.constEnd(); ++it) v.append({it.key(), it.value()});
    return v;
}

bool CrewPacks::remove(const QString& name) {
    const QString path =
        dir() + QLatin1Char('/') + sanitizeSegment(name) + QStringLiteral(".json");
    if (!QFileInfo(path).isFile()) return false;
    return QFile::remove(path);
}

}  // namespace odv
