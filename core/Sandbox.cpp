#include "Sandbox.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QVector>
#include <QtConcurrent>

#include <algorithm>

namespace odv {
namespace {

// Anything bigger than this is copied but never diffed: a 200KB+ hunk is
// unreadable for a human and blows the auditor model's context budget. Same
// threshold as the PHP original so existing stores diff identically.
constexpr qint64 kMaxDiffBytes = 200000;
// A NUL inside the first 8KB is the classic "this is not text" test (git uses
// the same idea). Cheap, and wrong only for files nobody reviews by eye.
constexpr int kBinarySniffBytes = 8000;

// Edit-script effort cap for one middle-snake search. Real edits need a handful
// of steps; if two versions of a file need more than this to reconcile they
// share almost nothing, and a replacement hunk is both cheaper to compute and
// easier to read than a minimal-but-shredded diff. Without a cap, a fully
// rewritten 200KB file would cost O(N*M) and hang the capture.
constexpr int kMaxSnakeD = 3000;

// excludes() returns by value, so begin()/end() on two separate calls would be
// iterators into two different temporaries. Materialise it once.
QSet<QString> excludeSet() {
    const QStringList ex = Sandbox::excludes();
    return QSet<QString>(ex.begin(), ex.end());
}

QByteArray readAll(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

bool writeAll(const QString& path, const QByteArray& data) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(data) == data.size();
}

bool looksBinary(const QByteArray& a, const QByteArray& b) {
    return a.left(kBinarySniffBytes).contains('\0') || b.left(kBinarySniffBytes).contains('\0');
}

// QDir::mkpath is not documented as thread-safe and the parallel workers all
// create store subdirectories; one mutex costs nothing next to the I/O.
QMutex g_mkdirMutex;
bool mkParents(const QString& filePath) {
    const QString parent = QFileInfo(filePath).absolutePath();
    QMutexLocker lock(&g_mkdirMutex);
    return QDir().mkpath(parent);
}

// Recursive walk with pruning. Symlinks are skipped entirely: following them
// risks cycles (and copying through one would write outside the sandbox); the
// PHP iterator did not descend into them either.
void walk(const QString& root, const QString& rel, const QSet<QString>& excl,
          QHash<QString, QString>* files, QStringList* dirs) {
    const QString abs = rel.isEmpty() ? root : root + '/' + rel;
    QDir dir(abs);
    const QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& fi : entries) {
        if (excl.contains(fi.fileName())) continue;  // excluded at ANY depth
        if (fi.isSymLink()) continue;
        const QString childRel = rel.isEmpty() ? fi.fileName() : rel + '/' + fi.fileName();
        if (fi.isDir()) {
            if (dirs) dirs->append(childRel);
            walk(root, childRel, excl, files, dirs);
        } else if (fi.isFile() && files) {
            files->insert(childRel, fi.absoluteFilePath());
        }
    }
}

// ---- unified diff -----------------------------------------------------------

QStringList splitLines(const QString& text, bool* noEolAtEof) {
    *noEolAtEof = !text.isEmpty() && !text.endsWith(QLatin1Char('\n'));
    if (text.isEmpty()) return {};
    QStringList lines = text.split(QLatin1Char('\n'));
    if (!*noEolAtEof) lines.removeLast();  // trailing "\n" yields a phantom empty line
    return lines;
}

// Myers' middle snake (the linear-space refinement of the O(ND) algorithm).
// Returns the snake's start (sx,sy) and end (ex,ey) in absolute indices, or
// false when the edit distance exceeds kMaxSnakeD.
bool middleSnake(const QVector<int>& a, int a0, int a1, const QVector<int>& b, int b0, int b1,
                 int* sx, int* sy, int* ex, int* ey) {
    const int n = a1 - a0;
    const int m = b1 - b0;
    const int sum = n + m;
    const int maxD = std::min((sum + 1) / 2, kMaxSnakeD);
    const int off = sum + 1;  // k is in [-maxD, maxD]; this shift keeps it in range
    const int delta = n - m;
    const bool odd = (delta & 1) != 0;

    QVector<int> vf(2 * sum + 3, 0);
    QVector<int> vb(2 * sum + 3, 0);
    vf[off + 1] = 0;
    vb[off + 1] = 0;

    for (int d = 0; d <= maxD; ++d) {
        for (int k = -d; k <= d; k += 2) {
            int x = (k == -d || (k != d && vf[off + k - 1] < vf[off + k + 1])) ? vf[off + k + 1]
                                                                              : vf[off + k - 1] + 1;
            int y = x - k;
            const int x0 = x, y0 = y;
            while (x < n && y < m && a[a0 + x] == b[b0 + y]) { ++x; ++y; }
            vf[off + k] = x;
            // A forward diagonal k is the same diagonal as reverse diagonal delta-k.
            const int kr = delta - k;
            if (odd && kr >= -(d - 1) && kr <= (d - 1) && x + vb[off + kr] >= n) {
                *sx = a0 + x0; *sy = b0 + y0; *ex = a0 + x; *ey = b0 + y;
                return true;
            }
        }
        for (int k = -d; k <= d; k += 2) {
            int x = (k == -d || (k != d && vb[off + k - 1] < vb[off + k + 1])) ? vb[off + k + 1]
                                                                              : vb[off + k - 1] + 1;
            int y = x - k;
            const int x0 = x, y0 = y;
            while (x < n && y < m && a[a0 + n - x - 1] == b[b0 + m - y - 1]) { ++x; ++y; }
            vb[off + k] = x;
            const int kf = delta - k;
            if (!odd && kf >= -d && kf <= d && x + vf[off + kf] >= n) {
                *sx = a0 + n - x; *sy = b0 + m - y; *ex = a0 + n - x0; *ey = b0 + m - y0;
                return true;
            }
        }
    }
    return false;
}

// Marks which lines of a were deleted and which of b were inserted.
void diffRec(const QVector<int>& a, int a0, int a1, const QVector<int>& b, int b0, int b1,
             QVector<bool>* aMod, QVector<bool>* bMod) {
    while (a0 < a1 && b0 < b1 && a[a0] == b[b0]) { ++a0; ++b0; }
    while (a0 < a1 && b0 < b1 && a[a1 - 1] == b[b1 - 1]) { --a1; --b1; }
    if (a0 == a1) {
        for (int j = b0; j < b1; ++j) (*bMod)[j] = true;
        return;
    }
    if (b0 == b1) {
        for (int i = a0; i < a1; ++i) (*aMod)[i] = true;
        return;
    }
    int sx = 0, sy = 0, ex = 0, ey = 0;
    const bool found = middleSnake(a, a0, a1, b, b0, b1, &sx, &sy, &ex, &ey);
    // No snake within the effort cap, or a degenerate one that would not shrink
    // the problem: fall back to "replace this whole region" rather than recurse
    // forever. Correct output, just not the minimal script.
    if (!found || (sx == a0 && sy == b0 && ex == a1 && ey == b1)) {
        for (int i = a0; i < a1; ++i) (*aMod)[i] = true;
        for (int j = b0; j < b1; ++j) (*bMod)[j] = true;
        return;
    }
    diffRec(a, a0, sx, b, b0, sy, aMod, bMod);
    diffRec(a, ex, a1, b, ey, b1, aMod, bMod);
}

struct Op {
    char tag;  // ' ' context, '-' deleted, '+' inserted
    int ai;    // index into old lines, -1 for '+'
    int bi;    // index into new lines, -1 for '-'
};

// The binary/oversize stub. Keeps the `diff --git` header — the secret scanner
// and the UI both key off it to count and colorize files.
QString binaryStub(const QString& path) {
    return QStringLiteral("diff --git a/%1 b/%1\nBinary files a/%1 and b/%1 differ\n").arg(path);
}

}  // namespace

QStringList Sandbox::excludes() {
    // Beyond the obvious VCS/vendor dirs, these are here because a coder that
    // RUNS its own work (a python import, a test) leaves interpreter and test
    // caches behind in the sandbox. Those are not the coder's changes, and
    // without this they get captured into the changeset and land in the user's
    // project on accept.
    return {QStringLiteral(".git"),
            QStringLiteral(".hg"),
            QStringLiteral(".svn"),
            QStringLiteral(".ollamadev"),
            QStringLiteral("node_modules"),
            QStringLiteral(".DS_Store"),
            QStringLiteral("ollamadev-crew"),
            QStringLiteral("__pycache__"),
            QStringLiteral(".pytest_cache"),
            QStringLiteral(".mypy_cache"),
            QStringLiteral(".ruff_cache"),
            QStringLiteral(".gradle"),
            QStringLiteral(".venv")};
}

QHash<QString, QString> Sandbox::listFiles(const QString& root) {
    QHash<QString, QString> out;
    if (!QFileInfo(root).isDir()) return out;
    walk(QDir(root).absolutePath(), QString(), excludeSet(), &out, nullptr);
    return out;
}

bool Sandbox::copyTree(const QString& src, const QString& dst, QString* err) {
    const QString from = QDir(src).absolutePath();
    const QString to = QDir(dst).absolutePath();
    if (!QFileInfo(from).isDir()) {
        if (err) *err = QStringLiteral("source is not a directory: ") + from;
        return false;
    }
    QHash<QString, QString> files;
    QStringList dirs;
    walk(from, QString(), excludeSet(), &files, &dirs);

    // Directories first, serially: cheap, and it means the parallel file jobs
    // never have to create a directory (no mkdir storm, no races).
    if (!QDir().mkpath(to)) {
        if (err) *err = QStringLiteral("cannot create ") + to;
        return false;
    }
    std::sort(dirs.begin(), dirs.end());
    for (const QString& d : dirs) {
        if (!QDir().mkpath(to + '/' + d)) {
            if (err) *err = QStringLiteral("cannot create ") + to + '/' + d;
            return false;
        }
    }

    // The whole point of the rewrite: N sandboxes used to be copied serially
    // before any coder could start. Copying is pure blocking I/O, so fan it out.
    const QStringList rels = files.keys();
    const QVector<QString> jobs(rels.begin(), rels.end());
    const auto copyOne = [&files, &to](const QString& rel) -> QString {
        const QString target = to + '/' + rel;
        QFile::remove(target);  // QFile::copy refuses to overwrite
        QFile in(files.value(rel));
        if (!in.copy(target)) return QStringLiteral("copy failed: ") + rel + " (" + in.errorString() + ")";
        // Preserve mtime so capture()'s size+mtime prefilter can tell an
        // untouched sandbox file from one a coder rewrote. Without this every
        // copy looks newer than the original and the prefilter never fires.
        const QDateTime mtime = QFileInfo(files.value(rel)).lastModified();
        QFile out(target);
        if (out.open(QIODevice::ReadWrite)) out.setFileTime(mtime, QFileDevice::FileModificationTime);
        return QString();
    };

    const QVector<QString> errs = QtConcurrent::blockingMapped<QVector<QString>>(jobs, copyOne);
    for (const QString& e : errs) {
        if (!e.isEmpty()) {
            if (err) *err = e;
            return false;
        }
    }
    return true;
}

bool Sandbox::removeTree(const QString& dir) {
    QFileInfo fi(dir);
    if (!fi.exists()) return true;
    if (!fi.isDir()) return QFile::remove(dir);
    return QDir(dir).removeRecursively();
}

Changeset Sandbox::capture(const QString& projectRoot, const QString& sandbox,
                           const QString& storeDir) {
    Changeset cs;
    cs.store = storeDir;

    const QString root = QDir(projectRoot).absolutePath();
    const QHash<QString, QString> proj = listFiles(root);
    const QHash<QString, QString> sand = listFiles(sandbox);

    removeTree(storeDir);
    QDir().mkpath(storeDir + QStringLiteral("/files"));

    // Deterministic order in manifest + diff: the crew's output feeds an
    // auditor model and a human reviewer, both of whom deserve stable output.
    QStringList sandRels = sand.keys();
    std::sort(sandRels.begin(), sandRels.end());
    QStringList delRels;
    for (auto it = proj.constBegin(); it != proj.constEnd(); ++it)
        if (!sand.contains(it.key())) delRels.append(it.key());
    std::sort(delRels.begin(), delRels.end());

    enum Status { Unchanged, Created, Modified, Deleted };
    struct Job {
        QString rel;
        QString sandAbs;  // empty => deletion
        QString projAbs;  // empty => creation
        QString store;
    };
    struct Result {
        QString rel;
        int status = Unchanged;
        QString diff;
    };

    QVector<Job> jobs;
    jobs.reserve(sandRels.size() + delRels.size());
    for (const QString& rel : sandRels)
        jobs.append({rel, sand.value(rel), proj.value(rel), storeDir});
    for (const QString& rel : delRels) jobs.append({rel, QString(), proj.value(rel), storeDir});

    const auto classify = [](const Job& j) -> Result {
        Result r;
        r.rel = j.rel;

        if (j.sandAbs.isEmpty()) {  // present in the project, gone from the sandbox
            r.status = Deleted;
            const QByteArray oldBytes = readAll(j.projAbs);
            r.diff = (oldBytes.size() > kMaxDiffBytes || looksBinary(oldBytes, QByteArray()))
                         ? binaryStub(j.rel)
                         : Sandbox::unifiedDiff(j.rel, QString::fromUtf8(oldBytes), QString());
            return r;
        }

        const bool isNew = j.projAbs.isEmpty();
        if (!isNew) {
            // Prefilter: the PHP version byte-compared every file of both trees on
            // every capture. A coder touches a handful of files, so trust the
            // rsync quick-check (same size AND same mtime => unchanged) and skip
            // the read entirely. copyTree() preserves mtime to make this sound;
            // any mismatch still falls through to a full content compare below.
            const QFileInfo si(j.sandAbs);
            const QFileInfo pi(j.projAbs);
            if (si.size() == pi.size() && si.lastModified() == pi.lastModified()) return r;
        }

        const QByteArray newBytes = readAll(j.sandAbs);
        const QByteArray oldBytes = isNew ? QByteArray() : readAll(j.projAbs);
        if (!isNew && oldBytes == newBytes) return r;  // touched but identical
        r.status = isNew ? Created : Modified;

        r.diff = (newBytes.size() > kMaxDiffBytes || oldBytes.size() > kMaxDiffBytes ||
                  looksBinary(newBytes, oldBytes))
                     ? binaryStub(j.rel)
                     : Sandbox::unifiedDiff(j.rel, QString::fromUtf8(oldBytes),
                                            QString::fromUtf8(newBytes));

        // Copy the full contents into the store: accept may run hours later,
        // long after the sandbox is gone.
        const QString target = j.store + QStringLiteral("/files/") + j.rel;
        mkParents(target);
        QFile::remove(target);
        QFile(j.sandAbs).copy(target);
        return r;
    };

    // Reading + diffing is the expensive part; fan it out too.
    const QVector<Result> results = QtConcurrent::blockingMapped<QVector<Result>>(jobs, classify);

    QString diff;
    for (const Result& r : results) {
        switch (r.status) {
            case Created: cs.created.append(r.rel); break;
            case Modified: cs.modified.append(r.rel); break;
            case Deleted: cs.deleted.append(r.rel); break;
            default: continue;
        }
        diff += r.diff;
    }
    cs.diff = diff;

    QJsonObject manifest;
    manifest.insert(QStringLiteral("created"), QJsonArray::fromStringList(cs.created));
    manifest.insert(QStringLiteral("modified"), QJsonArray::fromStringList(cs.modified));
    manifest.insert(QStringLiteral("deleted"), QJsonArray::fromStringList(cs.deleted));
    manifest.insert(QStringLiteral("projectRoot"), root);
    writeAll(storeDir + QStringLiteral("/manifest.json"),
             QJsonDocument(manifest).toJson(QJsonDocument::Compact));
    writeAll(storeDir + QStringLiteral("/diff.txt"), diff.toUtf8());
    return cs;
}

bool Sandbox::apply(const QString& storeDir, const QString& projectRoot, QStringList* wrote,
                    QString* err) {
    const QByteArray raw = readAll(storeDir + QStringLiteral("/manifest.json"));
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (err) *err = QStringLiteral("changeset missing or unreadable");
        return false;
    }
    const QJsonObject m = doc.object();
    const QString root = QDir(projectRoot).absolutePath();
    if (!QFileInfo(root).isDir()) {
        if (err) *err = QStringLiteral("project folder not found: ") + root;
        return false;
    }

    // Security requirement, not an optimisation: this is the ONE function that
    // writes into the user's tree, and the manifest is data an LLM's sandbox
    // produced. A rel path of "../../.ssh/authorized_keys" must never resolve.
    const auto resolve = [&root](const QString& rel, QString* abs) -> bool {
        if (rel.isEmpty() || QDir::isAbsolutePath(rel)) return false;
        const QString clean = QDir::cleanPath(rel);
        if (clean.isEmpty() || clean == QStringLiteral("..") || QDir::isAbsolutePath(clean)) return false;
        if (clean.startsWith(QStringLiteral("../")) || clean.contains(QStringLiteral("/../")) ||
            clean.endsWith(QStringLiteral("/..")))
            return false;
        const QString candidate = QDir::cleanPath(root + '/' + clean);
        if (candidate != root && !candidate.startsWith(root + '/')) return false;  // escaped
        *abs = candidate;
        return true;
    };

    QStringList written;
    QStringList touched;
    for (const QString& key : {QStringLiteral("created"), QStringLiteral("modified")})
        for (const QJsonValue& v : m.value(key).toArray()) touched.append(v.toString());

    for (const QString& rel : touched) {
        QString to;
        if (!resolve(rel, &to)) {
            if (err) *err = QStringLiteral("refusing to write outside the project: ") + rel;
            return false;
        }
        const QString from = storeDir + QStringLiteral("/files/") + rel;
        if (!QFileInfo(from).isFile()) continue;  // store pruned; nothing to write
        if (!QDir().mkpath(QFileInfo(to).absolutePath())) {
            if (err) *err = QStringLiteral("cannot create folder for ") + rel;
            return false;
        }
        QFile::remove(to);
        if (!QFile(from).copy(to)) {
            if (err) *err = QStringLiteral("cannot write ") + rel;
            return false;
        }
        written.append(rel);
    }

    for (const QJsonValue& v : m.value(QStringLiteral("deleted")).toArray()) {
        QString to;
        if (!resolve(v.toString(), &to)) {
            if (err) *err = QStringLiteral("refusing to delete outside the project: ") + v.toString();
            return false;
        }
        QFile::remove(to);  // already-gone is not an error
    }

    if (wrote) *wrote = written;
    return true;
}

QString Sandbox::unifiedDiff(const QString& path, const QString& oldText, const QString& newText,
                             int context) {
    QString out = QStringLiteral("diff --git a/%1 b/%1\n").arg(path);
    if (oldText == newText) return out;
    if (context < 0) context = 0;

    bool oldNoEol = false, newNoEol = false;
    const QStringList oldLines = splitLines(oldText, &oldNoEol);
    const QStringList newLines = splitLines(newText, &newNoEol);

    // The `new file mode` / `deleted file mode` lines are not decoration: without
    // them `git apply` strips /dev/null with -p1 and rejects the patch. 100644 is
    // an assumption — unifiedDiff only sees text, not the file's real mode.
    if (oldText.isEmpty())
        out += QStringLiteral("new file mode 100644\n--- /dev/null\n+++ b/%1\n").arg(path);
    else if (newText.isEmpty())
        out += QStringLiteral("deleted file mode 100644\n--- a/%1\n+++ /dev/null\n").arg(path);
    else
        out += QStringLiteral("--- a/%1\n+++ b/%1\n").arg(path);

    // Intern lines to ints: the diff compares each pair many times, and int
    // compares beat QString compares by a wide margin on large files.
    //
    // A final line with no trailing newline is NOT the same line as one with it,
    // or a file that only gained/lost its last newline would diff to nothing.
    // The sentinel gives it a distinct identity so it shows up as a -/+ pair.
    QHash<QString, int> ids;
    const auto idOf = [&ids](const QString& s, bool bare) {
        const QString key = bare ? s + QChar(0x1) : s;
        const auto it = ids.constFind(key);
        if (it != ids.constEnd()) return it.value();
        const int id = int(ids.size());
        ids.insert(key, id);
        return id;
    };
    QVector<int> a, b;
    a.reserve(oldLines.size());
    b.reserve(newLines.size());
    for (int k = 0; k < oldLines.size(); ++k)
        a.append(idOf(oldLines[k], oldNoEol && k == oldLines.size() - 1));
    for (int k = 0; k < newLines.size(); ++k)
        b.append(idOf(newLines[k], newNoEol && k == newLines.size() - 1));

    QVector<bool> aMod(a.size(), false), bMod(b.size(), false);
    diffRec(a, 0, a.size(), b, 0, b.size(), &aMod, &bMod);

    QVector<Op> ops;
    ops.reserve(a.size() + b.size());
    int i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        if (i < a.size() && j < b.size() && !aMod[i] && !bMod[j]) {
            ops.append({' ', i, j});
            ++i;
            ++j;
            continue;
        }
        while (i < a.size() && aMod[i]) ops.append({'-', i++, -1});
        while (j < b.size() && bMod[j]) ops.append({'+', -1, j++});
    }

    const auto emitLine = [&](const Op& op) {
        if (op.tag == '+') {
            out += QLatin1Char('+') + newLines[op.bi] + QLatin1Char('\n');
            if (newNoEol && op.bi == newLines.size() - 1)
                out += QStringLiteral("\\ No newline at end of file\n");
        } else {
            out += QLatin1Char(op.tag) + oldLines[op.ai] + QLatin1Char('\n');
            if (oldNoEol && op.ai == oldLines.size() - 1)
                out += QStringLiteral("\\ No newline at end of file\n");
        }
    };

    QVector<int> changes;
    for (int k = 0; k < ops.size(); ++k)
        if (ops[k].tag != ' ') changes.append(k);

    // Merge two changes into one hunk when fewer than 2*context unchanged lines
    // separate them — otherwise their context windows would overlap and git
    // would print the same lines twice.
    int gi = 0;
    while (gi < changes.size()) {
        const int first = changes[gi];
        int last = first;
        int gj = gi + 1;
        while (gj < changes.size() && (changes[gj] - last - 1) <= 2 * context) {
            last = changes[gj];
            ++gj;
        }
        gi = gj;
        const int lo = std::max(0, first - context);
        const int hi = std::min(int(ops.size()) - 1, last + context);

        int aStart = 0, bStart = 0, aCount = 0, bCount = 0;
        for (int k = 0; k < lo; ++k) {
            if (ops[k].tag != '+') ++aStart;
            if (ops[k].tag != '-') ++bStart;
        }
        for (int k = lo; k <= hi; ++k) {
            if (ops[k].tag != '+') ++aCount;
            if (ops[k].tag != '-') ++bCount;
        }
        // git prints the line BEFORE the range when a side is empty (new/deleted
        // file), which is exactly the 0-based count of preceding lines.
        out += QStringLiteral("@@ -%1,%2 +%3,%4 @@\n")
                   .arg(aCount ? aStart + 1 : aStart)
                   .arg(aCount)
                   .arg(bCount ? bStart + 1 : bStart)
                   .arg(bCount);
        for (int k = lo; k <= hi; ++k) emitLine(ops[k]);
    }
    return out;
}

}  // namespace odv
