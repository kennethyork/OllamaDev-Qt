#include "Plugins.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QUrl>

namespace odv {
namespace {

// A plugin name becomes a DIRECTORY NAME. Anything that could climb out of the
// plugins dir — a slash, a "..", an absolute path — is not a name. (The PHP
// `plugin remove` passed its argument straight into unlink(), so `../../foo` was
// live. It is not live here.)
QString sanitize(const QString& raw) {
    QString s;
    for (const QChar& c : raw.trimmed()) {
        if (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_') ||
            c == QLatin1Char('.'))
            s += c;
    }
    while (s.startsWith(QLatin1Char('.'))) s.remove(0, 1);  // no dotfiles, no ".."
    return s.left(64);
}

QJsonObject readJson(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

bool writeJson(const QString& path, const QJsonObject& o) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return f.commit();
}

// Which plugins the user has said yes to. Kept OUT of the plugin's own directory
// on purpose: a plugin must not be able to enable itself by shipping a manifest
// that says it is enabled, and a `git pull` inside the plugin must not be able to
// flip the bit either.
QString registryPath() { return Plugins::dir() + QStringLiteral("/enabled.json"); }

bool isEnabled(const QString& name) {
    return readJson(registryPath()).value(name).toBool(false);
}

bool copyTree(const QString& from, const QString& to) {
    QDir src(from);
    if (!src.exists()) return false;
    QDir().mkpath(to);
    for (const QFileInfo& fi :
         src.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        // Never copy a .git directory in: it is the bulk of the bytes and none of
        // the plugin.
        if (fi.isDir() && fi.fileName() == QLatin1String(".git")) continue;
        const QString dst = to + QLatin1Char('/') + fi.fileName();
        if (fi.isDir()) {
            if (!copyTree(fi.absoluteFilePath(), dst)) return false;
        } else {
            QFile::remove(dst);
            if (!QFile::copy(fi.absoluteFilePath(), dst)) return false;
        }
    }
    return true;
}

Plugin parse(const QString& pluginDir) {
    Plugin p;
    const QJsonObject m = readJson(pluginDir + QStringLiteral("/plugin.json"));
    p.dir = pluginDir;
    p.name = m.value(QStringLiteral("name")).toString(QFileInfo(pluginDir).fileName());
    p.version = m.value(QStringLiteral("version")).toString();
    p.description = m.value(QStringLiteral("description")).toString();
    p.homepage = m.value(QStringLiteral("homepage")).toString();
    p.enabled = isEnabled(p.name);

    for (const QJsonValue& v : m.value(QStringLiteral("hooks")).toArray()) {
        const QJsonObject o = v.toObject();
        PluginHook h;
        h.event = o.value(QStringLiteral("event")).toString();
        h.command = o.value(QStringLiteral("command")).toString();
        h.matcher = o.value(QStringLiteral("matcher")).toString();
        if (!h.event.isEmpty() && !h.command.isEmpty()) p.hooks.append(h);
    }
    p.mcp = m.value(QStringLiteral("mcp")).toObject();
    p.hasSkills = QFileInfo(pluginDir + QStringLiteral("/skills")).isDir();
    p.hasCommands = QFileInfo(pluginDir + QStringLiteral("/commands")).isDir();
    return p;
}

}  // namespace

QString Plugin::capabilities() const {
    QStringList bits;
    if (hasSkills) bits << QStringLiteral("skills");
    if (hasCommands) bits << QStringLiteral("commands");
    if (!mcp.isEmpty()) bits << QStringLiteral("%1 MCP server(s)").arg(mcp.size());
    // Said last and said plainly: this is the one that runs commands on your machine.
    if (!hooks.isEmpty())
        bits << QStringLiteral("%1 HOOK(S) — shell commands run on your machine").arg(hooks.size());
    return bits.isEmpty() ? QStringLiteral("nothing") : bits.join(QStringLiteral(" · "));
}

QString Plugins::dir() {
    return QDir::homePath() + QStringLiteral("/.ollamadev/plugins");
}

QVector<Plugin> Plugins::all() {
    QVector<Plugin> out;
    QDir d(dir());
    if (!d.exists()) return out;
    for (const QString& sub : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString pdir = dir() + QLatin1Char('/') + sub;
        if (!QFileInfo(pdir + QStringLiteral("/plugin.json")).isFile()) continue;
        out.append(parse(pdir));
    }
    return out;
}

QVector<Plugin> Plugins::active() {
    QVector<Plugin> out;
    for (const Plugin& p : all())
        if (p.enabled) out.append(p);
    return out;
}

bool Plugins::get(const QString& name, Plugin* out) {
    for (const Plugin& p : all()) {
        if (p.name.compare(name, Qt::CaseInsensitive) != 0) continue;
        if (out) *out = p;
        return true;
    }
    return false;
}

bool Plugins::install(const QString& source, QString* err, QString* installedName) {
    const QString src = source.trimmed();
    if (src.isEmpty()) {
        if (err) *err = QStringLiteral("give a directory or an https git URL");
        return false;
    }

    QString staged;             // where the plugin's files are before we validate them
    QDir tmp(QDir::tempPath() + QStringLiteral("/ollamadev-plugin-stage"));
    tmp.removeRecursively();

    if (QFileInfo(src).isDir()) {
        staged = QFileInfo(src).canonicalFilePath();
    } else {
        const QUrl url(src);
        // https only. A plugin can register shell hooks; fetching one over a
        // channel anybody on the path can rewrite is not a risk worth taking, and
        // "it also supports http" is not a feature anyone needs.
        if (url.scheme() != QLatin1String("https")) {
            if (err) *err = QStringLiteral("only a local directory or an https:// git URL");
            return false;
        }
        QDir().mkpath(tmp.path());
        // argv array, no shell. --depth 1: we want the files, not the history.
        QProcess git;
        git.setProgram(QStringLiteral("git"));
        git.setArguments({QStringLiteral("clone"), QStringLiteral("--depth"), QStringLiteral("1"),
                          src, tmp.path()});
        git.start();
        if (!git.waitForStarted(10000) || !git.waitForFinished(120000) || git.exitCode() != 0) {
            if (err) *err = QStringLiteral("could not clone %1").arg(src);
            tmp.removeRecursively();
            return false;
        }
        staged = tmp.path();
    }

    const QJsonObject m = readJson(staged + QStringLiteral("/plugin.json"));
    if (m.isEmpty()) {
        if (err) *err = QStringLiteral("no plugin.json — that is what makes it a plugin");
        tmp.removeRecursively();
        return false;
    }
    const QString name = sanitize(m.value(QStringLiteral("name")).toString());
    if (name.isEmpty()) {
        if (err) *err = QStringLiteral("plugin.json has no usable \"name\"");
        tmp.removeRecursively();
        return false;
    }

    const QString dest = dir() + QLatin1Char('/') + name;
    QDir(dest).removeRecursively();  // reinstall = replace
    if (!copyTree(staged, dest)) {
        if (err) *err = QStringLiteral("could not write %1").arg(dest);
        tmp.removeRecursively();
        return false;
    }
    tmp.removeRecursively();

    // Installed, and DELIBERATELY not enabled. Nothing in the plugin has run, and
    // nothing of it is live, until the user looks at what it wants and says yes.
    if (installedName) *installedName = name;
    return true;
}

bool Plugins::remove(const QString& name, QString* err) {
    Plugin p;
    if (!get(name, &p)) {
        if (err) *err = QStringLiteral("no plugin '%1'").arg(name);
        return false;
    }
    // p.dir came from scanning our OWN directory, never from user input, so this
    // cannot be pointed at an arbitrary path.
    if (!QDir(p.dir).removeRecursively()) {
        if (err) *err = QStringLiteral("could not remove %1").arg(p.dir);
        return false;
    }
    QJsonObject reg = readJson(registryPath());
    reg.remove(p.name);
    writeJson(registryPath(), reg);
    return true;
}

bool Plugins::setEnabled(const QString& name, bool on, QString* err) {
    Plugin p;
    if (!get(name, &p)) {
        if (err) *err = QStringLiteral("no plugin '%1'").arg(name);
        return false;
    }
    QJsonObject reg = readJson(registryPath());
    if (on)
        reg.insert(p.name, true);
    else
        reg.remove(p.name);
    if (!writeJson(registryPath(), reg)) {
        if (err) *err = QStringLiteral("could not write %1").arg(registryPath());
        return false;
    }
    return true;
}

QStringList Plugins::skillDirs() {
    QStringList out;
    for (const Plugin& p : active())
        if (p.hasSkills) out << p.dir + QStringLiteral("/skills");
    return out;
}

QStringList Plugins::commandDirs() {
    QStringList out;
    for (const Plugin& p : active())
        if (p.hasCommands) out << p.dir + QStringLiteral("/commands");
    return out;
}

QVector<PluginHook> Plugins::hooksFor(const QString& event) {
    QVector<PluginHook> out;
    for (const Plugin& p : active())
        for (const PluginHook& h : p.hooks)
            if (h.event.compare(event, Qt::CaseInsensitive) == 0) out.append(h);
    return out;
}

QJsonObject Plugins::mcpServers() {
    QJsonObject out;
    for (const Plugin& p : active())
        for (auto it = p.mcp.constBegin(); it != p.mcp.constEnd(); ++it)
            if (!out.contains(it.key())) out.insert(it.key(), it.value());
    return out;
}

}  // namespace odv
