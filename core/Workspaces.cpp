#include "Workspaces.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include "Config.h"
#include "Json.h"

namespace odv {
namespace {

QString storePath() {
    return QDir::homePath() + QStringLiteral("/.ollamadev/workspaces.json");
}

// ~ expands; a path that does not exist yet is kept AS TYPED rather than being
// silently collapsed to empty by canonicalFilePath() — a workspace on a drive you
// have not mounted today is still a workspace.
QString resolve(const QString& raw) {
    QString p = raw.trimmed();
    if (p.isEmpty()) p = QDir::currentPath();
    if (p == QLatin1String("~"))
        p = QDir::homePath();
    else if (p.startsWith(QLatin1String("~/")))
        p = QDir::homePath() + p.mid(1);
    const QFileInfo fi(p);
    return fi.exists() ? fi.canonicalFilePath() : QDir::cleanPath(fi.absoluteFilePath());
}

QString idFor(const QString& absPath) {
    return QStringLiteral("ws_") +
           QString::fromLatin1(
               QCryptographicHash::hash(absPath.toUtf8(), QCryptographicHash::Sha1).toHex().left(10));
}

QJsonObject loadFile() {
    QFile f(storePath());
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

bool saveFile(const QJsonObject& o) {
    QDir().mkpath(QFileInfo(storePath()).absolutePath());
    QSaveFile f(storePath());
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return f.commit();  // atomic: a half-written workspaces.json loses every bookmark
}

Workspace fromJson(const QJsonObject& o) {
    Workspace w;
    w.id = o.value(QStringLiteral("id")).toString();
    w.name = o.value(QStringLiteral("name")).toString();
    w.path = o.value(QStringLiteral("path")).toString();
    w.lastOpened = o.value(QStringLiteral("lastOpened")).toString();
    w.state = o.value(QStringLiteral("state")).toObject();
    return w;
}

QJsonObject toJson(const Workspace& w) {
    return QJsonObject{{"id", w.id},
                       {"name", w.name},
                       {"path", w.path},
                       {"lastOpened", w.lastOpened},
                       {"state", w.state}};
}

QString nowIso() { return QDateTime::currentDateTime().toString(Qt::ISODate); }

}  // namespace

QVector<Workspace> Workspaces::all() {
    QVector<Workspace> v;
    for (const QJsonValue& x : loadFile().value(QStringLiteral("workspaces")).toArray())
        if (x.isObject()) v.append(fromJson(x.toObject()));
    return v;
}

QString Workspaces::activeId() {
    return loadFile().value(QStringLiteral("active")).toString();
}

Workspace Workspaces::add(const QString& path, const QString& name) {
    const QString abs = resolve(path);
    QJsonObject root = loadFile();
    QJsonArray list = root.value(QStringLiteral("workspaces")).toArray();

    Workspace w;
    w.id = idFor(abs);
    w.path = abs;
    w.name = name.trimmed().isEmpty() ? QFileInfo(abs).fileName() : name.trimmed();
    w.lastOpened = nowIso();

    // Upsert. The id is derived from the path, so adding a folder you already track
    // updates it rather than making a second entry for the same directory.
    bool found = false;
    for (int i = 0; i < list.size(); ++i) {
        const QJsonObject o = list.at(i).toObject();
        if (o.value(QStringLiteral("id")).toString() != w.id) continue;
        Workspace old = fromJson(o);
        w.state = old.state;  // NEVER clobber the desktop's saved layout
        if (name.trimmed().isEmpty()) w.name = old.name;
        list.replace(i, toJson(w));
        found = true;
        break;
    }
    if (!found) list.append(toJson(w));

    root.insert(QStringLiteral("workspaces"), list);
    root.insert(QStringLiteral("active"), w.id);
    saveFile(root);
    return w;
}

bool Workspaces::find(const QString& key, Workspace* out) {
    if (key.trimmed().isEmpty()) return false;
    const QVector<Workspace> list = all();
    const QString abs = resolve(key);

    for (const Workspace& w : list)
        if (w.id == key) return out ? (*out = w, true) : true;
    for (const Workspace& w : list)
        if (w.path == abs) return out ? (*out = w, true) : true;
    for (const Workspace& w : list)
        if (w.name.compare(key, Qt::CaseInsensitive) == 0) return out ? (*out = w, true) : true;
    return false;
}

bool Workspaces::remove(const QString& key) {
    Workspace target;
    if (!find(key, &target)) return false;

    QJsonObject root = loadFile();
    QJsonArray list = root.value(QStringLiteral("workspaces")).toArray();
    QJsonArray kept;
    for (const QJsonValue& x : list)
        if (x.toObject().value(QStringLiteral("id")).toString() != target.id) kept.append(x);

    root.insert(QStringLiteral("workspaces"), kept);
    // Removing the active workspace must not leave `active` pointing at nothing.
    if (root.value(QStringLiteral("active")).toString() == target.id) {
        root.insert(QStringLiteral("active"),
                    kept.isEmpty() ? QJsonValue()
                                   : kept.first().toObject().value(QStringLiteral("id")));
    }
    return saveFile(root);
}

QString Workspaces::open(const QString& key) {
    Workspace w;
    if (!find(key, &w)) return {};

    QJsonObject root = loadFile();
    QJsonArray list = root.value(QStringLiteral("workspaces")).toArray();
    for (int i = 0; i < list.size(); ++i) {
        QJsonObject o = list.at(i).toObject();
        if (o.value(QStringLiteral("id")).toString() != w.id) continue;
        o.insert(QStringLiteral("lastOpened"), nowIso());
        list.replace(i, o);
        break;
    }
    root.insert(QStringLiteral("workspaces"), list);
    root.insert(QStringLiteral("active"), w.id);
    saveFile(root);
    return w.path;
}

bool Workspaces::saveState(const QString& id, const QJsonObject& state) {
    QJsonObject root = loadFile();
    QJsonArray list = root.value(QStringLiteral("workspaces")).toArray();
    for (int i = 0; i < list.size(); ++i) {
        QJsonObject o = list.at(i).toObject();
        if (o.value(QStringLiteral("id")).toString() != id) continue;
        o.insert(QStringLiteral("state"), state);
        list.replace(i, o);
        root.insert(QStringLiteral("workspaces"), list);
        return saveFile(root);
    }
    return false;
}

}  // namespace odv
