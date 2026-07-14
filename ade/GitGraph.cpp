#include "GitGraph.h"

namespace odv {

QVector<GraphCommit> GitGraph::parse(const QString& logOutput) {
    QVector<GraphCommit> out;
    for (const QString& line : logOutput.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        // %H|%P|%an|%ad|%D|%s — the subject may itself contain '|', so split with a
        // cap and let the last field keep the rest.
        const QStringList f = line.split(QLatin1Char('|'));
        if (f.size() < 6) continue;
        GraphCommit c;
        c.sha = f.at(0);
        c.parents = f.at(1).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        c.author = f.at(2);
        c.date = f.at(3);
        for (const QString& r : f.at(4).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString t = r.trimmed();
            if (t.isEmpty()) continue;
            c.refs << t;
            if (t.startsWith(QLatin1String("HEAD"))) c.isHead = true;
        }
        c.subject = QStringList(f.mid(5)).join(QLatin1Char('|'));
        out.append(c);
    }
    return out;
}

int GitGraph::layout(QVector<GraphCommit>& commits) {
    // lanes[i] = the sha that lane i is currently waiting to draw. Empty = free.
    QVector<QString> lanes;
    int widest = 1;

    const auto findLane = [&lanes](const QString& sha) {
        for (int i = 0; i < lanes.size(); ++i)
            if (lanes.at(i) == sha) return i;
        return -1;
    };
    const auto freeLane = [&lanes]() -> int {
        for (int i = 0; i < lanes.size(); ++i)
            if (lanes.at(i).isEmpty()) return i;
        lanes.append(QString());
        return static_cast<int>(lanes.size()) - 1;
    };

    for (GraphCommit& c : commits) {
        // Which lane is already waiting for this commit? (Its child claimed one for
        // it.) If none, this is a branch tip: take a fresh lane.
        int lane = findLane(c.sha);
        if (lane < 0) {
            lane = freeLane();
            lanes[lane] = c.sha;
        }
        c.lane = lane;

        // Every OTHER live lane passes straight through this row — that is what
        // draws the vertical lines beside a commit.
        for (int i = 0; i < lanes.size(); ++i)
            if (i != lane && !lanes.at(i).isEmpty()) c.links.append({i, i});

        // The first parent inherits this lane, so a linear history draws a straight
        // line. Any further parent (a merge) gets its own lane and an edge into it.
        if (c.parents.isEmpty()) {
            lanes[lane].clear();  // a root commit: the lane ends here
        } else {
            lanes[lane] = c.parents.first();
            for (int p = 1; p < c.parents.size(); ++p) {
                const QString& par = c.parents.at(p);
                int pl = findLane(par);
                if (pl < 0) {
                    pl = freeLane();
                    lanes[pl] = par;
                }
                c.links.append({lane, pl});  // the merge edge
            }
        }

        // A lane whose sha is already drawn elsewhere is a duplicate (two children
        // of one parent); collapse it so the graph does not grow a lane per child.
        for (int i = 0; i < lanes.size(); ++i) {
            if (lanes.at(i).isEmpty()) continue;
            for (int j = i + 1; j < lanes.size(); ++j) {
                if (lanes.at(j) != lanes.at(i)) continue;
                c.links.append({j, i});
                lanes[j].clear();
            }
        }

        int live = 0;
        for (const QString& l : lanes)
            if (!l.isEmpty()) ++live;
        c.lanesWide = qMax(1, qMax(live, lane + 1));
        widest = qMax(widest, c.lanesWide);
    }
    return widest;
}
}  // namespace odv
