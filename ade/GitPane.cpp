#include "GitPane.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QSet>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTextEdit>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <thread>

#include "Config.h"
#include "GitFlow.h"
#include "GitGraph.h"
#include "Rebase.h"
#include "RebaseDialog.h"
#include "SecScan.h"
#include "Theme.h"

namespace odv {
namespace {

// A git client on the canvas.
//
// Everything runs through GitFlow::git, which spawns git with an argv ARRAY in the
// tool root. Nothing here builds a shell string, so a branch name with a space or
// a semicolon in it is a branch name, not syntax.
//
// Two rules the whole pane is built around:
//
//   * The commit button enforces the SAME secret gate the CLI does. A desktop
//     button that quietly skipped a gate the CLI applies would be worse than no
//     button at all.
//   * Every destructive action asks first, and says what it is about to destroy.
//     A hard reset, a branch -D, a discard and a force-push are all one click from
//     here. The difference between a git tool you trust and one you fear is
//     whether it tells you before it eats your work.

struct Change {
    QString path;
    QString code;
    bool staged = false;
};

// `git status --porcelain`: index status in column 0, worktree status in column 1.
// A file can appear in BOTH lists (staged one hunk, edited again since), which is
// the whole reason the two columns exist and why we do not collapse them.
QVector<Change> parseStatus(const QString& out) {
    QVector<Change> v;
    for (const QString& raw : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        if (raw.size() < 4) continue;
        const QChar index = raw.at(0), tree = raw.at(1);
        QString path = raw.mid(3).trimmed();
        const int arrow = path.indexOf(QStringLiteral(" -> "));  // "R  old -> new"
        if (arrow >= 0) path = path.mid(arrow + 4);
        if (path.isEmpty()) continue;
        if (index != QLatin1Char(' ') && index != QLatin1Char('?'))
            v.append(Change{path, raw.left(2), true});
        if (tree != QLatin1Char(' ') || index == QLatin1Char('?'))
            v.append(Change{path, raw.left(2), false});
    }
    return v;
}

QString describeCode(const QString& code) {
    if (code.startsWith(QStringLiteral("??"))) return QStringLiteral("new");
    if (code.contains(QLatin1Char('U'))) return QStringLiteral("CONFLICT");
    const QChar c = code.trimmed().isEmpty() ? QLatin1Char(' ') : code.trimmed().at(0);
    switch (c.toLatin1()) {
        case 'M': return QStringLiteral("modified");
        case 'A': return QStringLiteral("added");
        case 'D': return QStringLiteral("deleted");
        case 'R': return QStringLiteral("renamed");
        case 'C': return QStringLiteral("copied");
        default: return QStringLiteral("changed");
    }
}

constexpr int kLaneW = 14;  // px per graph lane
constexpr int kRowH = 22;

// Stable colour per lane, so a branch keeps its colour while you scroll. Lives
// here rather than in GitGraph: the graph's job is deciding which lane a commit
// sits in, which is pure logic and testable without a GUI.
QColor laneColor(int lane) {
    static const QColor kColors[] = {
        QColor(0x4f, 0xc3, 0xf7), QColor(0x81, 0xc7, 0x84), QColor(0xff, 0xb7, 0x4d),
        QColor(0xba, 0x68, 0xc8), QColor(0xe5, 0x73, 0x73), QColor(0x4d, 0xd0, 0xe1),
        QColor(0xff, 0xd5, 0x4f), QColor(0xa1, 0x88, 0x7f),
    };
    return kColors[qAbs(lane) % 8];
}

// Paints the lane graph in column 0 of the history list.
//
// A delegate rather than a separate widget beside the list, so the graph and its
// rows can never scroll out of step — which is the single most annoying bug a git
// GUI can have.
class GraphDelegate : public QStyledItemDelegate {
public:
    explicit GraphDelegate(QObject* p = nullptr) : QStyledItemDelegate(p) {}

    void paint(QPainter* p, const QStyleOptionViewItem& o, const QModelIndex& i) const override {
        QStyledItemDelegate::paint(p, o, i);
        const QVariant v = i.data(Qt::UserRole + 2);
        if (!v.isValid()) return;
        const QVariantList packed = v.toList();
        if (packed.size() < 3) return;

        const int lane = packed.at(0).toInt();
        const QVariantList links = packed.at(1).toList();
        const bool isHead = packed.at(2).toBool();

        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        const int top = o.rect.top(), h = o.rect.height();
        const int midY = top + h / 2;
        const auto laneX = [&o](int l) { return o.rect.left() + 8 + l * kLaneW; };

        // Edges first, so a dot always sits on top of its own lines.
        for (const QVariant& e : links) {
            const QVariantList pair = e.toList();
            if (pair.size() != 2) continue;
            const int from = pair.at(0).toInt(), to = pair.at(1).toInt();
            p->setPen(QPen(laneColor(to), 1.6));
            if (from == to) {
                p->drawLine(laneX(from), top, laneX(from), top + h);  // straight through
            } else {
                QPainterPath path;  // a merge/branch edge curving into another lane
                path.moveTo(laneX(from), midY);
                path.cubicTo(laneX(from), midY + h / 2, laneX(to), midY, laneX(to), top + h);
                p->drawPath(path);
            }
        }

        const QColor c = laneColor(lane);
        p->setPen(QPen(c, 1.6));
        p->setBrush(isHead ? QBrush(c) : QBrush(Qt::NoBrush));
        const qreal r = isHead ? 4.5 : 3.5;
        p->drawEllipse(QPointF(laneX(lane), midY), r, r);
        p->restore();
    }
};

class GitWidget : public QWidget {
public:
    explicit GitWidget(PaneHost& host, QWidget* parent = nullptr) : QWidget(parent), host_(host) {
        // A canvas pane spawns at 540x340. A git client that DEMANDS 780x470 is a
        // git client you have to resize before you can use, every single time — so
        // this one has to work small and grow well, not the other way round.
        setMinimumSize(360, 260);
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(6, 6, 6, 6);
        root->setSpacing(6);

        // ---- toolbar --------------------------------------------------------
        auto* bar = new QHBoxLayout;
        branchLabel_ = new QLabel(this);
        branchLabel_->setStyleSheet(QStringLiteral("font-weight:600"));
        // `full` is the label a wide pane shows; a narrow one keeps the tooltip and
        // shows the initial. See relayout().
        const auto button = [this](const QString& label) {
            auto* b = new QPushButton(label, this);
            b->setProperty("full", label);
            b->setToolTip(label);
            return b;
        };
        tidyBtn_ = button(tr("Tidy"));
        tidyBtn_->setToolTip(tr("Tidy history — interactive rebase, with the model proposing "
                                "what to fold together and what to reword"));
        newBranchBtn_ = button(tr("Branch"));
        stashBtn_ = button(tr("Stash"));
        fetchBtn_ = button(tr("Fetch"));
        pullBtn_ = button(tr("Pull"));
        pushBtn_ = button(tr("Push"));
        auto* newBranch = newBranchBtn_;
        auto* stash = stashBtn_;
        auto* fetch = fetchBtn_;
        auto* pull = pullBtn_;
        auto* push = pushBtn_;
        bar->addWidget(branchLabel_);
        bar->addStretch(1);
        bar->addWidget(tidyBtn_);
        bar->addWidget(newBranchBtn_);
        bar->addWidget(stashBtn_);
        bar->addWidget(fetchBtn_);
        bar->addWidget(pullBtn_);
        bar->addWidget(pushBtn_);
        root->addLayout(bar);

        // ---- sidebar | graph | detail ---------------------------------------
        split_ = new QSplitter(Qt::Horizontal, this);
        split_->setChildrenCollapsible(true);  // the sidebar is the first thing to go

        side_ = new QTreeWidget(split_);
        side_->setHeaderHidden(true);
        side_->setMinimumWidth(110);
        side_->setContextMenuPolicy(Qt::CustomContextMenu);
        split_->addWidget(side_);

        auto* mid = new QWidget(split_);
        auto* mv = new QVBoxLayout(mid);
        mv->setContentsMargins(0, 0, 0, 0);
        mv->setSpacing(4);
        search_ = new QLineEdit(mid);
        search_->setPlaceholderText(tr("Search commits"));
        search_->setClearButtonEnabled(true);
        mv->addWidget(search_);
        history_ = new QTreeWidget(mid);
        history_->setHeaderLabels({tr("Graph"), tr("Commit"), tr("Author"), tr("When")});
        history_->setRootIsDecorated(false);
        history_->setUniformRowHeights(true);
        history_->setItemDelegateForColumn(0, new GraphDelegate(history_));
        history_->setContextMenuPolicy(Qt::CustomContextMenu);
        history_->header()->setStretchLastSection(false);
        // The message is the column you actually read, so it takes the slack. The
        // other three are sized to their content and never steal from it — which is
        // what squeezed the subject down to "[f…" when the pane was narrow.
        history_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        history_->setMinimumWidth(160);
        mv->addWidget(history_, 1);
        split_->addWidget(mid);

        right_ = new QTabWidget(split_);
        right_->setMinimumWidth(150);
        right_->addTab(buildWorking(), tr("Working"));
        right_->addTab(buildDetail(), tr("Commit"));
        split_->addWidget(right_);

        split_->setStretchFactor(0, 0);
        split_->setStretchFactor(1, 3);
        split_->setStretchFactor(2, 2);
        root->addWidget(split_, 1);

        status_ = new QLabel(this);
        status_->setWordWrap(true);
        status_->setStyleSheet(QStringLiteral("color:%1").arg(Theme::currentColors().dim.name()));
        root->addWidget(status_);

        connect(tidyBtn_, &QPushButton::clicked, this, [this] { tidyHistory(); });
        connect(newBranch, &QPushButton::clicked, this, [this] { createBranch(QString()); });
        connect(stash, &QPushButton::clicked, this, [this] { stashPush(); });
        connect(fetch, &QPushButton::clicked, this, [this] {
            run({QStringLiteral("fetch"), QStringLiteral("--all"), QStringLiteral("--prune")});
        });
        connect(pull, &QPushButton::clicked, this, [this] { run({QStringLiteral("pull")}); });
        connect(push, &QPushButton::clicked, this, [this] { pushMenu(); });
        connect(side_, &QTreeWidget::customContextMenuRequested, this,
                [this](const QPoint& p) { sidebarMenu(p); });
        connect(side_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* i, int) {
            if (kind(i) == QLatin1String("branch")) checkout(payload(i));
        });
        connect(history_, &QTreeWidget::customContextMenuRequested, this,
                [this](const QPoint& p) { historyMenu(p); });
        connect(history_, &QTreeWidget::currentItemChanged, this,
                [this](QTreeWidgetItem* i, QTreeWidgetItem*) { showCommit(i); });
        connect(search_, &QLineEdit::textChanged, this, [this](const QString&) { filterHistory(); });

        timer_ = new QTimer(this);
        timer_->setInterval(3000);
        connect(timer_, &QTimer::timeout, this, [this] { refresh(); });
    }

protected:
    void showEvent(QShowEvent* e) override {
        QWidget::showEvent(e);
        relayout();
        refresh();
        timer_->start();
    }
    void hideEvent(QHideEvent* e) override {
        timer_->stop();
        QWidget::hideEvent(e);
    }
    void resizeEvent(QResizeEvent* e) override {
        QWidget::resizeEvent(e);
        relayout();
    }

private:
    // Three columns side by side need room. Below that, stop pretending: hide the
    // sidebar (its whole content is reachable from the context menus anyway) and,
    // narrower still, stack the graph above the working area instead of squeezing
    // both into a column each. A git pane that only works maximised is a git pane
    // you resize before every use.
    void relayout() {
        const int w = width();
        const bool wide = w >= 620;
        const bool roomy = w >= 460;

        side_->setVisible(wide);
        split_->setOrientation(roomy ? Qt::Horizontal : Qt::Vertical);
        // A narrow pane hides the search box too — at that width it is competing
        // with the thing it searches.
        search_->setVisible(w >= 380);

        // Five full-width buttons alone want ~400px, which is most of a small pane.
        // Below that they lose their labels and keep their tooltips.
        const bool labels = w >= 560;
        for (auto* b : {stashBtn_, fetchBtn_, pullBtn_, pushBtn_, newBranchBtn_, tidyBtn_}) {
            b->setText(labels ? b->property("full").toString()
                              : b->property("full").toString().left(1));
            b->setFixedWidth(labels ? QWIDGETSIZE_MAX : 26);
        }
        branchLabel_->setVisible(w >= 300);

        // Author and date are the first things to go: the message and the graph are
        // what the column is for.
        history_->setColumnHidden(2, w < 560);
        history_->setColumnHidden(3, w < 500);

        if (wide) {
            split_->setSizes({150, qMax(200, w - 480), 300});
        } else if (roomy) {
            split_->setSizes({0, qMax(160, w / 2), qMax(160, w / 2)});
        } else {
            split_->setSizes({0, height() / 2, height() / 2});
        }
    }

    // ---- panels ------------------------------------------------------------

    QWidget* buildWorking() {
        auto* w = new QWidget(this);
        auto* v = new QVBoxLayout(w);
        v->setContentsMargins(4, 6, 4, 4);
        v->setSpacing(4);

        v->addWidget(new QLabel(tr("Staged"), w));
        staged_ = new QListWidget(w);
        staged_->setContextMenuPolicy(Qt::CustomContextMenu);
        v->addWidget(staged_, 1);

        v->addWidget(new QLabel(tr("Changes — double-click to stage"), w));
        unstaged_ = new QListWidget(w);
        unstaged_->setContextMenuPolicy(Qt::CustomContextMenu);
        v->addWidget(unstaged_, 1);

        auto* row = new QHBoxLayout;
        auto* stageAll = new QPushButton(tr("Stage all"), w);
        auto* unstageAll = new QPushButton(tr("Unstage all"), w);
        auto* discard = new QPushButton(tr("Discard all"), w);
        row->addWidget(stageAll);
        row->addWidget(unstageAll);
        row->addWidget(discard);
        v->addLayout(row);

        wdiff_ = new QTextEdit(w);
        wdiff_->setReadOnly(true);
        wdiff_->setLineWrapMode(QTextEdit::NoWrap);
        v->addWidget(wdiff_, 2);

        message_ = new QPlainTextEdit(w);
        message_->setPlaceholderText(tr("Commit message — or let the model write it"));
        message_->setMaximumHeight(64);
        v->addWidget(message_);

        auto* crow = new QHBoxLayout;
        amend_ = new QCheckBox(tr("Amend"), w);
        amend_->setToolTip(tr("Replace the previous commit instead of adding a new one"));
        ai_ = new QPushButton(tr("Write it for me"), w);
        commit_ = new QPushButton(tr("Commit"), w);
        commit_->setProperty("cta", true);
        crow->addWidget(amend_);
        crow->addStretch(1);
        crow->addWidget(ai_);
        crow->addWidget(commit_);
        v->addLayout(crow);

        connect(unstaged_, &QListWidget::itemDoubleClicked, this,
                [this](QListWidgetItem* i) { stage(i, true); });
        connect(unstaged_, &QListWidget::itemClicked, this,
                [this](QListWidgetItem* i) { showWorkingDiff(i, false); });
        connect(staged_, &QListWidget::itemDoubleClicked, this,
                [this](QListWidgetItem* i) { stage(i, false); });
        connect(staged_, &QListWidget::itemClicked, this,
                [this](QListWidgetItem* i) { showWorkingDiff(i, true); });
        connect(unstaged_, &QListWidget::customContextMenuRequested, this,
                [this](const QPoint& p) { fileMenu(unstaged_, p, false); });
        connect(staged_, &QListWidget::customContextMenuRequested, this,
                [this](const QPoint& p) { fileMenu(staged_, p, true); });
        connect(stageAll, &QPushButton::clicked, this, [this] {
            GitFlow::git({QStringLiteral("add"), QStringLiteral("-A")});
            refresh();
        });
        connect(unstageAll, &QPushButton::clicked, this, [this] {
            GitFlow::git({QStringLiteral("reset")});
            refresh();
        });
        connect(discard, &QPushButton::clicked, this, [this] { discardAll(); });
        connect(amend_, &QCheckBox::toggled, this, [this](bool) { refreshStatus(); });
        connect(ai_, &QPushButton::clicked, this, [this] { writeMessage(); });
        connect(commit_, &QPushButton::clicked, this, [this] { doCommit(); });
        return w;
    }

    QWidget* buildDetail() {
        auto* w = new QWidget(this);
        auto* v = new QVBoxLayout(w);
        v->setContentsMargins(4, 6, 4, 4);
        meta_ = new QLabel(w);
        meta_->setWordWrap(true);
        meta_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(meta_);
        files_ = new QListWidget(w);
        v->addWidget(files_, 1);
        cdiff_ = new QTextEdit(w);
        cdiff_->setReadOnly(true);
        cdiff_->setLineWrapMode(QTextEdit::NoWrap);
        v->addWidget(cdiff_, 2);
        connect(files_, &QListWidget::itemClicked, this, [this](QListWidgetItem* i) {
            if (!i || detailSha_.isEmpty()) return;
            paintDiff(cdiff_, GitFlow::git({QStringLiteral("--no-pager"), QStringLiteral("show"),
                                            detailSha_, QStringLiteral("--"), i->text()})
                                  .output);
        });
        return w;
    }

    // ---- refresh -----------------------------------------------------------

    void run(const QStringList& args) {
        const GitResult r = GitFlow::git(args);
        status_->setText(r.output.trimmed().left(400));
        lastLog_.clear();  // the graph almost certainly moved
        refresh();
    }

    void refresh() {
        if (busy_) return;
        if (!GitFlow::isRepo()) {
            status_->setText(tr("Not a git repository."));
            setEnabled(false);
            return;
        }
        setEnabled(true);
        branchName_ = GitFlow::branch();
        branchLabel_->setText(tr("On %1").arg(branchName_));
        refreshStatus();
        refreshSidebar();
        refreshHistory();
    }

    void refreshStatus() {
        // Do not fight the user: a list rebuilt every 3s that clears their selection
        // makes the diff view unreadable.
        const QString keepS = current(staged_), keepU = current(unstaged_);
        const QVector<Change> all = parseStatus(
            GitFlow::git({QStringLiteral("status"), QStringLiteral("--porcelain")}).output);
        staged_->clear();
        unstaged_->clear();
        for (const Change& c : all) {
            const QString what = describeCode(c.code);
            auto* it = new QListWidgetItem(QStringLiteral("%1  %2").arg(what.leftJustified(9), c.path));
            it->setData(Qt::UserRole, c.path);
            if (what == QLatin1String("CONFLICT")) it->setForeground(Theme::currentColors().err);
            (c.staged ? staged_ : unstaged_)->addItem(it);
        }
        reselect(staged_, keepS);
        reselect(unstaged_, keepU);
        // Amend with nothing staged is legitimate — it rewrites the message.
        commit_->setEnabled(staged_->count() > 0 || amend_->isChecked());
        ai_->setEnabled(staged_->count() > 0);
    }

    void refreshSidebar() {
        const QString sel = payload(side_->currentItem());
        QSet<QString> collapsed;
        for (int i = 0; i < side_->topLevelItemCount(); ++i)
            if (!side_->topLevelItem(i)->isExpanded())
                collapsed.insert(side_->topLevelItem(i)->text(0));

        side_->clear();
        const auto group = [&](const QString& name) {
            auto* g = new QTreeWidgetItem(side_, {name});
            g->setExpanded(!collapsed.contains(name));  // a tree that re-collapses itself
            QFont f = g->font(0);                        // every 3 seconds is unusable
            f.setBold(true);
            g->setFont(0, f);
            return g;
        };
        const auto add = [](QTreeWidgetItem* g, const QString& text, const QString& k,
                            const QString& data) {
            auto* it = new QTreeWidgetItem(g, {text});
            it->setData(0, Qt::UserRole, k);
            it->setData(0, Qt::UserRole + 1, data);
            return it;
        };
        const auto lines = [](const GitResult& r) {
            return r.output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        };

        auto* locals = group(tr("Branches"));
        for (const QString& b : lines(GitFlow::git(
                 {QStringLiteral("branch"), QStringLiteral("--format=%(refname:short)")}))) {
            auto* it = add(locals, b.trimmed(), QStringLiteral("branch"), b.trimmed());
            if (b.trimmed() != branchName_) continue;
            QFont f = it->font(0);
            f.setBold(true);
            it->setFont(0, f);
            it->setForeground(0, Theme::currentColors().accent);
        }

        auto* remotes = group(tr("Remote branches"));
        for (const QString& b :
             lines(GitFlow::git({QStringLiteral("branch"), QStringLiteral("-r"),
                                 QStringLiteral("--format=%(refname:short)")}))) {
            if (b.contains(QStringLiteral("HEAD"))) continue;  // origin/HEAD is a pointer, not a branch
            add(remotes, b.trimmed(), QStringLiteral("remote-branch"), b.trimmed());
        }

        auto* tags = group(tr("Tags"));
        for (const QString& t : lines(GitFlow::git({QStringLiteral("tag"), QStringLiteral("--list")})))
            add(tags, t.trimmed(), QStringLiteral("tag"), t.trimmed());

        auto* stashes = group(tr("Stashes"));
        for (const QString& s :
             lines(GitFlow::git({QStringLiteral("stash"), QStringLiteral("list")}))) {
            const int colon = s.indexOf(QLatin1Char(':'));
            add(stashes, s.trimmed(), QStringLiteral("stash"),
                colon > 0 ? s.left(colon) : s);  // "stash@{0}"
        }

        auto* rem = group(tr("Remotes"));
        QSet<QString> seen;
        for (const QString& line :
             lines(GitFlow::git({QStringLiteral("remote"), QStringLiteral("-v")}))) {
            const QString name = line.section(QLatin1Char('\t'), 0, 0).trimmed();
            if (name.isEmpty() || seen.contains(name)) continue;
            seen.insert(name);
            add(rem, line.simplified(), QStringLiteral("remote"), name);
        }

        if (sel.isEmpty()) return;
        QTreeWidgetItemIterator it(side_);
        while (*it) {
            if (payload(*it) == sel) {
                side_->setCurrentItem(*it);
                break;
            }
            ++it;
        }
    }

    void refreshHistory() {
        // --all, so branches you are NOT on are drawn too. A graph that only shows
        // the current branch is a list.
        const QString log = GitFlow::git({QStringLiteral("--no-pager"), QStringLiteral("log"),
                                          QStringLiteral("--all"), QStringLiteral("--parents"),
                                          QStringLiteral("--date=short"),
                                          QStringLiteral("--pretty=format:%H|%P|%an|%ad|%D|%s"),
                                          QStringLiteral("-n"), QStringLiteral("300")})
                                .output;
        if (log == lastLog_) return;  // unchanged: leave the user's selection alone
        lastLog_ = log;

        const QString keep = history_->currentItem()
                                 ? history_->currentItem()->data(1, Qt::UserRole).toString()
                                 : QString();

        commits_ = GitGraph::parse(log);
        const int widest = GitGraph::layout(commits_);

        history_->clear();
        for (const GraphCommit& c : commits_) {
            QString subject = c.subject;
            if (!c.refs.isEmpty()) {
                QStringList tidy;
                for (const QString& r : c.refs)
                    tidy << (r.startsWith(QLatin1String("HEAD -> ")) ? r.mid(8) : r);
                subject = QStringLiteral("[%1]  %2").arg(tidy.join(QStringLiteral(", ")), subject);
            }
            auto* it = new QTreeWidgetItem(history_, {QString(), subject, c.author, c.date});
            it->setData(1, Qt::UserRole, c.sha);

            QVariantList links;
            for (const auto& e : c.links) links << QVariant(QVariantList{e.first, e.second});
            it->setData(0, Qt::UserRole + 2, QVariantList{c.lane, links, c.isHead});
            it->setSizeHint(0, QSize(16 + widest * kLaneW, kRowH));
            if (!c.isHead) continue;
            QFont f = it->font(1);
            f.setBold(true);
            it->setFont(1, f);
        }
        history_->setColumnWidth(0, 16 + widest * kLaneW);
        history_->resizeColumnToContents(2);
        history_->resizeColumnToContents(3);
        filterHistory();

        if (keep.isEmpty()) return;
        for (int i = 0; i < history_->topLevelItemCount(); ++i) {
            if (history_->topLevelItem(i)->data(1, Qt::UserRole).toString() != keep) continue;
            history_->setCurrentItem(history_->topLevelItem(i));
            break;
        }
    }

    void filterHistory() {
        const QString q = search_->text().trimmed();
        for (int i = 0; i < history_->topLevelItemCount(); ++i) {
            QTreeWidgetItem* it = history_->topLevelItem(i);
            it->setHidden(!(q.isEmpty() || it->text(1).contains(q, Qt::CaseInsensitive) ||
                            it->text(2).contains(q, Qt::CaseInsensitive) ||
                            it->data(1, Qt::UserRole).toString().startsWith(q)));
        }
    }

    // ---- helpers -----------------------------------------------------------

    static QString kind(QTreeWidgetItem* i) {
        return i ? i->data(0, Qt::UserRole).toString() : QString();
    }
    static QString payload(QTreeWidgetItem* i) {
        return i ? i->data(0, Qt::UserRole + 1).toString() : QString();
    }
    static QString current(QListWidget* w) {
        auto* i = w->currentItem();
        return i ? i->data(Qt::UserRole).toString() : QString();
    }
    static void reselect(QListWidget* w, const QString& path) {
        if (path.isEmpty()) return;
        for (int i = 0; i < w->count(); ++i) {
            if (w->item(i)->data(Qt::UserRole).toString() != path) continue;
            w->setCurrentRow(i);
            return;
        }
    }

    // Every destructive action goes through here, and every one of them says what
    // it is about to destroy.
    bool confirm(const QString& title, const QString& text, const QString& okLabel) {
        QMessageBox box(host_.window());
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(title);
        box.setText(text);
        box.setStandardButtons(QMessageBox::Cancel);
        auto* go = box.addButton(okLabel, QMessageBox::DestructiveRole);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        return box.clickedButton() == go;
    }

    QString ask(const QString& title, const QString& label, const QString& initial = {}) {
        bool ok = false;
        const QString s = QInputDialog::getText(host_.window(), title, label, QLineEdit::Normal,
                                                initial, &ok);
        return ok ? s.trimmed() : QString();
    }

    // ---- working tree ------------------------------------------------------

    void stage(QListWidgetItem* item, bool add) {
        if (!item) return;
        const QString path = item->data(Qt::UserRole).toString();
        // `reset -- <path>` unstages WITHOUT touching the working tree. Never
        // `checkout --` here: that would delete the user's edit, and an "unstage"
        // button that destroys work is a trap.
        GitFlow::git(
            {add ? QStringLiteral("add") : QStringLiteral("reset"), QStringLiteral("--"), path});
        refresh();
    }

    void fileMenu(QListWidget* list, const QPoint& p, bool staged) {
        QListWidgetItem* it = list->itemAt(p);
        if (!it) return;
        const QString path = it->data(Qt::UserRole).toString();

        QMenu m(this);
        m.addAction(staged ? tr("Unstage") : tr("Stage"), this,
                    [this, it, staged] { stage(it, !staged); });
        m.addAction(tr("Open in editor"), this, [this, path] { host_.openFile(path); });
        m.addSeparator();
        m.addAction(tr("Discard changes…"), this, [this, path] {
            if (!confirm(tr("Discard"),
                         tr("Throw away every change to %1?\n\nIt goes back to how it was at the "
                            "last commit. This cannot be undone.")
                             .arg(path),
                         tr("Discard it")))
                return;
            GitFlow::git({QStringLiteral("checkout"), QStringLiteral("--"), path});
            // An untracked file has nothing to check out — it has to be removed.
            GitFlow::git({QStringLiteral("clean"), QStringLiteral("-f"), QStringLiteral("--"), path});
            refresh();
        });
        m.exec(list->mapToGlobal(p));
    }

    void discardAll() {
        if (!confirm(tr("Discard everything"),
                     tr("Throw away EVERY uncommitted change here, new files included?\n\nThis "
                        "cannot be undone."),
                     tr("Discard everything")))
            return;
        GitFlow::git({QStringLiteral("reset"), QStringLiteral("--hard")});
        GitFlow::git({QStringLiteral("clean"), QStringLiteral("-f"), QStringLiteral("-d")});
        refresh();
    }

    void showWorkingDiff(QListWidgetItem* item, bool staged) {
        if (!item) return;
        const QString path = item->data(Qt::UserRole).toString();
        QStringList args{QStringLiteral("--no-pager"), QStringLiteral("diff")};
        if (staged) args << QStringLiteral("--cached");
        args << QStringLiteral("--") << path;
        QString text = GitFlow::git(args).output;
        if (text.trimmed().isEmpty())
            // An untracked file has no diff — show its contents, which is what you
            // actually want to read before adding it.
            text = GitFlow::git({QStringLiteral("--no-pager"), QStringLiteral("diff"),
                                 QStringLiteral("--no-index"), QStringLiteral("/dev/null"), path})
                       .output;
        paintDiff(wdiff_, text);
    }

    void paintDiff(QTextEdit* into, const QString& text) {
        const Theme::Colors c = Theme::currentColors();
        QString html = QStringLiteral("<pre style='font-family:monospace;margin:0'>");
        for (const QString& line : text.split(QLatin1Char('\n'))) {
            QString col = c.fg.name();
            if (line.startsWith(QLatin1Char('+')) && !line.startsWith(QStringLiteral("+++")))
                col = c.ok.name();
            else if (line.startsWith(QLatin1Char('-')) && !line.startsWith(QStringLiteral("---")))
                col = c.err.name();
            else if (line.startsWith(QStringLiteral("@@")))
                col = c.accent.name();
            else if (line.startsWith(QStringLiteral("diff ")) ||
                     line.startsWith(QStringLiteral("index ")))
                col = c.dim.name();
            html +=
                QStringLiteral("<span style='color:%1'>%2</span>\n").arg(col, line.toHtmlEscaped());
        }
        into->setHtml(html + QStringLiteral("</pre>"));
    }

    // ---- commit ------------------------------------------------------------

    void writeMessage() {
        const QString diff = GitFlow::stagedDiff();
        if (diff.trimmed().isEmpty()) {
            status_->setText(tr("Nothing staged."));
            return;
        }
        setBusy(true, tr("asking %1…").arg(GitFlow::modelFor(host_.currentModel())));
        QPointer<GitWidget> self = this;
        const QString backend = host_.currentBackend(), model = host_.currentModel();
        std::thread([self, diff, backend, model] {
            CancelToken cancel;
            const QString msg =
                GitFlow::commitMessage(diff, backend, GitFlow::modelFor(model), cancel);
            QMetaObject::invokeMethod(qApp, [self, msg] {
                if (!self) return;
                self->setBusy(false, QString());
                if (msg.trimmed().isEmpty()) {
                    self->status_->setText(tr("The model could not write a message."));
                    return;
                }
                self->message_->setPlainText(msg);
            });
        }).detach();
    }

    void doCommit() {
        const QString msg = message_->toPlainText().trimmed();
        if (msg.isEmpty()) {
            status_->setText(tr("Write a message first (or let the model)."));
            return;
        }

        // THE GATE — the same one the CLI enforces.
        const QVector<Finding> bad = GitFlow::highFindings(GitFlow::stagedDiff());
        if (!bad.isEmpty()) {
            QStringList lines;
            for (const Finding& f : bad)
                lines << QStringLiteral("· %1 — %2").arg(f.rule, f.redacted);
            if (!confirm(tr("A secret is staged"),
                         tr("The staged diff looks like it contains a credential:\n\n%1\n\nA pushed "
                            "secret must be treated as leaked — rotate it, do not just amend it "
                            "away.")
                             .arg(lines.join(QLatin1Char('\n'))),
                         tr("Commit anyway"))) {
                status_->setText(tr("Blocked: a secret is staged."));
                return;
            }
        }

        QStringList args{QStringLiteral("commit")};
        if (amend_->isChecked()) args << QStringLiteral("--amend");
        // The message goes in over STDIN, not argv: it is multi-line, and it may have
        // been written by a model reading a diff that came from a repo you cloned.
        args << QStringLiteral("-F") << QStringLiteral("-");
        const GitResult r = GitFlow::git(args, msg);
        if (!r.ok()) {
            status_->setText(r.output.trimmed().left(400));
            return;
        }
        message_->clear();
        amend_->setChecked(false);
        status_->setText(tr("Committed."));
        lastLog_.clear();
        refresh();
    }

    void pushMenu() {
        QMenu m(this);
        m.addAction(tr("Push"), this, [this] { run({QStringLiteral("push")}); });
        m.addAction(tr("Push and set upstream"), this, [this] {
            run({QStringLiteral("push"), QStringLiteral("--set-upstream"), QStringLiteral("origin"),
                 branchName_});
        });
        m.addSeparator();
        m.addAction(tr("Force push (with lease)…"), this, [this] {
            // --force-with-lease, never a bare --force: it still lets you rewrite a
            // branch you own, but refuses when the remote moved under you — which is
            // exactly the case where a plain --force destroys someone else's commits.
            if (!confirm(tr("Force push"),
                         tr("Force-push %1?\n\nThis REWRITES the remote branch. It is refused if "
                            "the remote moved since you last fetched — but whatever you replaced "
                            "locally will be gone from the remote.")
                             .arg(branchName_),
                         tr("Force push")))
                return;
            run({QStringLiteral("push"), QStringLiteral("--force-with-lease")});
        });
        m.exec(QCursor::pos());
    }

    // ---- branches / tags / stashes / remotes -------------------------------

    void checkout(const QString& ref) { run({QStringLiteral("checkout"), ref}); }

    void tidyHistory() {
        // A rebase mid-rebase is not a thing. Offer the way out instead.
        if (Rebase::inProgress()) {
            if (confirm(tr("Rebase in progress"),
                        tr("A rebase is already running and stopped on something.\n\nAbort it and "
                           "put everything back?"),
                        tr("Abort it")))
                run({QStringLiteral("rebase"), QStringLiteral("--abort")});
            return;
        }
        RebaseDialog dlg(host_, host_.window());
        if (dlg.exec() == QDialog::Accepted) {
            lastLog_.clear();
            refresh();
        }
    }

    void createBranch(const QString& from) {
        const QString name =
            ask(tr("New branch"),
                tr("Branch name (starting from %1):").arg(from.isEmpty() ? branchName_ : from.left(8)));
        if (name.isEmpty()) return;
        QStringList args{QStringLiteral("checkout"), QStringLiteral("-b"), name};
        if (!from.isEmpty()) args << from;
        run(args);
    }

    void stashPush() {
        const QString msg = ask(tr("Stash"), tr("Describe it (optional):"));
        QStringList args{QStringLiteral("stash"), QStringLiteral("push"),
                         // Untracked files are NOT stashed by default, which surprises
                         // everybody exactly once, usually painfully.
                         QStringLiteral("--include-untracked")};
        if (!msg.isEmpty()) args << QStringLiteral("-m") << msg;
        run(args);
    }

    void sidebarMenu(const QPoint& p) {
        QTreeWidgetItem* it = side_->itemAt(p);
        const QString k = kind(it), data = payload(it);
        if (k.isEmpty()) return;

        QMenu m(this);
        if (k == QLatin1String("branch")) {
            const bool isCurrent = data == branchName_;
            if (!isCurrent) m.addAction(tr("Check out"), this, [this, data] { checkout(data); });
            m.addAction(tr("New branch from here…"), this, [this, data] { createBranch(data); });
            if (!isCurrent) {
                m.addSeparator();
                m.addAction(tr("Merge into %1").arg(branchName_), this,
                            [this, data] { run({QStringLiteral("merge"), data}); });
                m.addAction(tr("Rebase %1 onto this").arg(branchName_), this,
                            [this, data] { run({QStringLiteral("rebase"), data}); });
            }
            m.addAction(tr("Rename…"), this, [this, data] {
                const QString name = ask(tr("Rename branch"), tr("New name:"), data);
                if (!name.isEmpty())
                    run({QStringLiteral("branch"), QStringLiteral("-m"), data, name});
            });
            if (!isCurrent) {
                m.addSeparator();
                m.addAction(tr("Delete…"), this, [this, data] { deleteBranch(data); });
            }
        } else if (k == QLatin1String("remote-branch")) {
            m.addAction(tr("Check out as a local branch"), this, [this, data] {
                run({QStringLiteral("checkout"), QStringLiteral("-b"),
                     data.section(QLatin1Char('/'), 1), data});
            });
            m.addAction(tr("Merge into %1").arg(branchName_), this,
                        [this, data] { run({QStringLiteral("merge"), data}); });
        } else if (k == QLatin1String("tag")) {
            m.addAction(tr("Check out"), this, [this, data] { checkout(data); });
            m.addAction(tr("Push tag"), this, [this, data] {
                run({QStringLiteral("push"), QStringLiteral("origin"), data});
            });
            m.addAction(tr("Delete"), this, [this, data] {
                run({QStringLiteral("tag"), QStringLiteral("-d"), data});
            });
        } else if (k == QLatin1String("stash")) {
            m.addAction(tr("Apply (and keep the stash)"), this, [this, data] {
                run({QStringLiteral("stash"), QStringLiteral("apply"), data});
            });
            m.addAction(tr("Pop (apply and drop it)"), this, [this, data] {
                run({QStringLiteral("stash"), QStringLiteral("pop"), data});
            });
            m.addAction(tr("Drop…"), this, [this, data] {
                if (confirm(tr("Drop stash"),
                            tr("Throw away %1?\n\nThis cannot be undone.").arg(data), tr("Drop it")))
                    run({QStringLiteral("stash"), QStringLiteral("drop"), data});
            });
        } else if (k == QLatin1String("remote")) {
            m.addAction(tr("Fetch"), this, [this, data] {
                run({QStringLiteral("fetch"), data, QStringLiteral("--prune")});
            });
            m.addAction(tr("Remove"), this, [this, data] {
                run({QStringLiteral("remote"), QStringLiteral("remove"), data});
            });
        }
        if (!m.isEmpty()) m.exec(side_->mapToGlobal(p));
    }

    void deleteBranch(const QString& b) {
        // -d refuses to drop unmerged work; -D does not. Try the safe one first, and
        // only offer the unsafe one when git itself objects — that way the warning
        // the user reads is GIT's, about their actual repository, not a guess of ours.
        const GitResult r = GitFlow::git({QStringLiteral("branch"), QStringLiteral("-d"), b});
        if (r.ok()) {
            lastLog_.clear();
            refresh();
            return;
        }
        if (!confirm(tr("Delete branch"),
                     tr("git refuses:\n\n%1\n\nDelete it anyway? Commits that live only on this "
                        "branch become unreachable.")
                         .arg(r.output.trimmed()),
                     tr("Delete anyway")))
            return;
        run({QStringLiteral("branch"), QStringLiteral("-D"), b});
    }

    // ---- history -----------------------------------------------------------

    void showCommit(QTreeWidgetItem* it) {
        if (!it) return;
        const QString sha = it->data(1, Qt::UserRole).toString();
        if (sha.isEmpty() || sha == detailSha_) return;
        detailSha_ = sha;
        right_->setCurrentIndex(1);

        meta_->setText(GitFlow::git({QStringLiteral("--no-pager"), QStringLiteral("show"),
                                     QStringLiteral("--no-patch"),
                                     QStringLiteral("--pretty=format:%h  %an <%ae>  %ad%n%n%B"), sha})
                           .output.trimmed()
                           .left(800));

        files_->clear();
        for (const QString& f :
             GitFlow::git({QStringLiteral("--no-pager"), QStringLiteral("show"),
                           QStringLiteral("--name-only"), QStringLiteral("--pretty=format:"), sha})
                 .output.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            files_->addItem(f.trimmed());

        // Capped: a merge commit's full diff can be megabytes, and painting it as
        // HTML would lock the UI thread for seconds.
        paintDiff(cdiff_, GitFlow::git({QStringLiteral("--no-pager"), QStringLiteral("show"), sha})
                              .output.left(200000));
    }

    void historyMenu(const QPoint& p) {
        QTreeWidgetItem* it = history_->itemAt(p);
        if (!it) return;
        const QString sha = it->data(1, Qt::UserRole).toString();
        if (sha.isEmpty()) return;
        const QString shortSha = sha.left(8);

        QMenu m(this);
        m.addAction(tr("Copy sha"), this,
                    [sha] { QApplication::clipboard()->setText(sha); });
        m.addAction(tr("Check out this commit"), this, [this, sha] {
            run({QStringLiteral("checkout"), sha});  // detached HEAD; git says so loudly
        });
        m.addAction(tr("New branch here…"), this, [this, sha] { createBranch(sha); });
        m.addAction(tr("Tag here…"), this, [this, sha] {
            const QString name = ask(tr("New tag"), tr("Tag name:"));
            if (!name.isEmpty()) run({QStringLiteral("tag"), name, sha});
        });
        m.addSeparator();
        m.addAction(tr("Cherry-pick onto %1").arg(branchName_), this,
                    [this, sha] { run({QStringLiteral("cherry-pick"), sha}); });
        m.addAction(tr("Revert — a new commit that undoes this one"), this, [this, sha] {
            run({QStringLiteral("revert"), QStringLiteral("--no-edit"), sha});
        });
        m.addSeparator();

        // The three resets, named for what they DO. "Mixed" and "hard" mean nothing
        // to most people; "keep my changes" and "throw my changes away" each mean
        // exactly one thing, and the difference between them is somebody's evening.
        m.addAction(tr("Reset %1 here — keep my changes, staged").arg(branchName_), this,
                    [this, sha] { run({QStringLiteral("reset"), QStringLiteral("--soft"), sha}); });
        m.addAction(tr("Reset %1 here — keep my changes, unstaged").arg(branchName_), this,
                    [this, sha] { run({QStringLiteral("reset"), QStringLiteral("--mixed"), sha}); });
        m.addAction(tr("Reset %1 here — THROW AWAY my changes…").arg(branchName_), this,
                    [this, sha, shortSha] {
                        if (!confirm(tr("Hard reset"),
                                     tr("Move %1 to %2 and DELETE every uncommitted change?\n\nThis "
                                        "cannot be undone.")
                                         .arg(branchName_, shortSha),
                                     tr("Throw it away")))
                            return;
                        run({QStringLiteral("reset"), QStringLiteral("--hard"), sha});
                    });
        m.exec(history_->mapToGlobal(p));
    }

    void setBusy(bool on, const QString& msg) {
        busy_ = on;
        ai_->setEnabled(!on && staged_->count() > 0);
        commit_->setEnabled(!on && (staged_->count() > 0 || amend_->isChecked()));
        status_->setText(msg);
    }

    PaneHost& host_;
    QLabel* branchLabel_;
    QPushButton* newBranchBtn_;
    QPushButton* tidyBtn_;
    QPushButton* stashBtn_;
    QPushButton* fetchBtn_;
    QPushButton* pullBtn_;
    QPushButton* pushBtn_;
    QSplitter* split_;
    QTreeWidget* side_;
    QTreeWidget* history_;
    QLineEdit* search_;
    QTabWidget* right_;
    QListWidget* staged_;
    QListWidget* unstaged_;
    QListWidget* files_;
    QTextEdit* wdiff_;
    QTextEdit* cdiff_;
    QLabel* meta_;
    QPlainTextEdit* message_;
    QCheckBox* amend_;
    QPushButton* ai_;
    QPushButton* commit_;
    QLabel* status_;
    QTimer* timer_;
    QString branchName_;
    QString detailSha_;
    QString lastLog_;
    QVector<GraphCommit> commits_;
    bool busy_ = false;
};

}  // namespace

PaneSpec makeGitPaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("git");
    s.title = QStringLiteral("Git");
    s.group = QStringLiteral("Tools");
    s.singleton = true;
    s.factory = [](PaneHost& h) -> QWidget* { return new GitWidget(h); };
    return s;
}

}  // namespace odv
