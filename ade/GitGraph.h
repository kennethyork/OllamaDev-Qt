#pragma once
#include <QColor>
#include <QString>
#include <QStringList>
#include <QVector>

namespace odv {

// One commit, with the lane geometry needed to draw the graph.
struct GraphCommit {
    QString sha;
    QStringList parents;
    QString author;
    QString date;
    QString subject;
    QStringList refs;  // "main", "origin/main", "tag: v1.0", "HEAD -> main"
    bool isHead = false;

    // Assigned by layout():
    int lane = 0;                  // which column this commit's dot sits in
    QVector<QPair<int, int>> links;  // (fromLane, toLane) edges passing through this row
    int lanesWide = 1;             // how many lanes are live at this row
};

// The commit graph — the thing that makes a git client feel like a git client.
//
// Lane assignment is the classic "keep a list of open branch tips" sweep: walk the
// commits newest-first; a commit takes the lane of whichever open tip is waiting
// for it (or a fresh lane if nothing is), then its parents replace it — the first
// parent inherits the lane so a straight line stays straight, and any second
// parent (a merge) opens a lane of its own.
//
// It is O(commits x lanes) and honest about what it is: a topological drawing of
// what `git log --parents` said, not an opinion about it.
class GitGraph {
public:
    // `git log --all --parents --date=short --pretty=%H|%P|%an|%ad|%D|%s`
    static QVector<GraphCommit> parse(const QString& logOutput);

    // Fills in lane / links / lanesWide. Returns the widest row.
    static int layout(QVector<GraphCommit>& commits);

    // Stable colour per lane, so a branch keeps its colour as you scroll.
    static QColor laneColor(int lane);
};

}  // namespace odv
