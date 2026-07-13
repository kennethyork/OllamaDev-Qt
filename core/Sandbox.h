#pragma once
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace odv {

// A changeset: what a coder actually changed, captured by comparing its
// sandbox against the project. Durable, so a human can accept it hours later.
//
// On disk under <store>/:
//   files/<rel path>   full contents of every created/modified file
//   manifest.json      {created:[], modified:[], deleted:[], projectRoot:""}
//   diff.txt           unified diff, for review and the secret scanner
struct Changeset {
    QStringList created;
    QStringList modified;
    QStringList deleted;
    QString diff;
    QString store;
    bool empty() const { return created.isEmpty() && modified.isEmpty() && deleted.isEmpty(); }
    QStringList files() const { return created + modified + deleted; }
};

class Sandbox {
public:
    // Directories never copied into a sandbox nor compared.
    static QStringList excludes();

    // Full recursive copy. Parallelised across files — this is pure I/O and was
    // one of the serial bottlenecks in the PHP version.
    static bool copyTree(const QString& src, const QString& dst, QString* err = nullptr);
    static bool removeTree(const QString& dir);

    // rel path -> absolute, leaves only, excludes applied at any depth.
    static QHash<QString, QString> listFiles(const QString& root);

    // Compare `sandbox` against `projectRoot`, write the changeset to `storeDir`.
    static Changeset capture(const QString& projectRoot, const QString& sandbox,
                             const QString& storeDir);

    // Copy a stored changeset into the project. Creates parent folders.
    // This is what "accept" runs — it is the only thing that ever writes to the
    // user's tree.
    static bool apply(const QString& storeDir, const QString& projectRoot,
                      QStringList* wrote = nullptr, QString* err = nullptr);

    // Real unified diff (Myers), not a whole-file replacement hunk. Keeps the
    // auditor's context budget usable on large files.
    static QString unifiedDiff(const QString& path, const QString& oldText,
                               const QString& newText, int context = 3);
};

}  // namespace odv
