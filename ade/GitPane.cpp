#include "GitPane.h"

#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

#include <thread>

#include "Config.h"
#include "GitFlow.h"
#include "SecScan.h"
#include "Theme.h"

namespace odv {
namespace {

// A git client on the canvas — the desktop half of the workflow the CLI already
// had (`ollamadev commit` / `ship`).
//
// Everything here runs through GitFlow::git, which spawns git with an argv ARRAY
// in the tool root. Nothing on this pane builds a shell string, so a branch name
// with a space or a semicolon in it is a branch name, not syntax.
//
// The commit button is deliberately the same commit the CLI does: SecScan runs on
// the STAGED diff first and a high-severity credential blocks it. A desktop button
// that quietly skipped the gate the CLI enforces would be worse than no button.

struct Change {
    QString path;
    QString code;  // the two-letter git porcelain status, e.g. " M", "A ", "??"
    bool staged = false;
};

// `git status --porcelain` — index status in column 0, worktree status in column 1.
// A file can appear in BOTH lists (staged one hunk, edited again since), which is
// the whole reason the two columns exist and why we do not collapse them.
QVector<Change> parseStatus(const QString& out) {
    QVector<Change> v;
    for (const QString& raw : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        if (raw.size() < 4) continue;
        const QChar index = raw.at(0), tree = raw.at(1);
        QString path = raw.mid(3).trimmed();
        // A rename reads "R  old -> new"; the new name is the one you can act on.
        const int arrow = path.indexOf(QStringLiteral(" -> "));
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
    const QChar c = code.trimmed().isEmpty() ? QLatin1Char(' ') : code.trimmed().at(0);
    if (code.startsWith(QStringLiteral("??"))) return QStringLiteral("new");
    switch (c.toLatin1()) {
        case 'M': return QStringLiteral("modified");
        case 'A': return QStringLiteral("added");
        case 'D': return QStringLiteral("deleted");
        case 'R': return QStringLiteral("renamed");
        case 'C': return QStringLiteral("copied");
        case 'U': return QStringLiteral("conflicted");
        default: return QStringLiteral("changed");
    }
}

class GitWidget : public QWidget {
public:
    explicit GitWidget(PaneHost& host, QWidget* parent = nullptr)
        : QWidget(parent), host_(host) {
        setMinimumSize(520, 380);

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(8, 8, 8, 8);
        root->setSpacing(6);

        // ---- branch bar ----------------------------------------------------
        auto* bar = new QHBoxLayout;
        branch_ = new QComboBox(this);
        branch_->setMinimumWidth(160);
        branch_->setToolTip(tr("Switch branch"));
        auto* fetch = new QPushButton(tr("Fetch"), this);
        auto* pull = new QPushButton(tr("Pull"), this);
        push_ = new QPushButton(tr("Push"), this);
        bar->addWidget(new QLabel(tr("Branch:"), this));
        bar->addWidget(branch_);
        bar->addStretch(1);
        bar->addWidget(fetch);
        bar->addWidget(pull);
        bar->addWidget(push_);
        root->addLayout(bar);

        // ---- changes | diff -------------------------------------------------
        auto* tabs = new QTabWidget(this);

        auto* changes = new QWidget(tabs);
        auto* cv = new QHBoxLayout(changes);
        cv->setContentsMargins(0, 6, 0, 0);

        auto* lists = new QWidget(changes);
        auto* lv = new QVBoxLayout(lists);
        lv->setContentsMargins(0, 0, 0, 0);
        lv->addWidget(new QLabel(tr("Staged — will be committed"), lists));
        staged_ = new QListWidget(lists);
        lv->addWidget(staged_, 1);
        lv->addWidget(new QLabel(tr("Changes — click to stage"), lists));
        unstaged_ = new QListWidget(lists);
        lv->addWidget(unstaged_, 1);
        auto* stageRow = new QHBoxLayout;
        auto* stageAll = new QPushButton(tr("Stage all"), lists);
        auto* unstageAll = new QPushButton(tr("Unstage all"), lists);
        stageRow->addWidget(stageAll);
        stageRow->addWidget(unstageAll);
        lv->addLayout(stageRow);

        diff_ = new QTextEdit(changes);
        diff_->setReadOnly(true);
        diff_->setLineWrapMode(QTextEdit::NoWrap);
        diff_->setFontFamily(QStringLiteral("monospace"));

        auto* split = new QSplitter(Qt::Horizontal, changes);
        split->addWidget(lists);
        split->addWidget(diff_);
        split->setStretchFactor(1, 1);
        cv->addWidget(split);
        tabs->addTab(changes, tr("Changes"));

        auto* history = new QWidget(tabs);
        auto* hv = new QVBoxLayout(history);
        hv->setContentsMargins(0, 6, 0, 0);
        log_ = new QListWidget(history);
        hv->addWidget(log_, 1);
        tabs->addTab(history, tr("History"));
        root->addWidget(tabs, 1);

        // ---- commit ----------------------------------------------------------
        message_ = new QPlainTextEdit(this);
        message_->setPlaceholderText(tr("Commit message — or let the model write it"));
        message_->setMaximumHeight(70);
        root->addWidget(message_);

        auto* row = new QHBoxLayout;
        ai_ = new QPushButton(tr("✨ Write it for me"), this);
        ai_->setToolTip(tr("Have the model write a Conventional Commit message for the staged diff"));
        commit_ = new QPushButton(tr("Commit"), this);
        commit_->setProperty("cta", true);
        status_ = new QLabel(this);
        status_->setWordWrap(true);
        row->addWidget(status_, 1);
        row->addWidget(ai_);
        row->addWidget(commit_);
        root->addLayout(row);

        connect(unstaged_, &QListWidget::itemActivated, this,
                [this](QListWidgetItem* i) { stage(i, true); });
        connect(unstaged_, &QListWidget::itemClicked, this,
                [this](QListWidgetItem* i) { showDiff(i, false); });
        connect(staged_, &QListWidget::itemActivated, this,
                [this](QListWidgetItem* i) { stage(i, false); });
        connect(staged_, &QListWidget::itemClicked, this,
                [this](QListWidgetItem* i) { showDiff(i, true); });
        connect(stageAll, &QPushButton::clicked, this, [this] {
            GitFlow::git({QStringLiteral("add"), QStringLiteral("-A")});
            refresh();
        });
        connect(unstageAll, &QPushButton::clicked, this, [this] {
            GitFlow::git({QStringLiteral("reset")});
            refresh();
        });
        connect(ai_, &QPushButton::clicked, this, [this] { writeMessage(); });
        connect(commit_, &QPushButton::clicked, this, [this] { doCommit(); });
        connect(fetch, &QPushButton::clicked, this, [this] { run({QStringLiteral("fetch")}); });
        connect(pull, &QPushButton::clicked, this, [this] { run({QStringLiteral("pull")}); });
        connect(push_, &QPushButton::clicked, this, [this] { doPush(); });
        connect(branch_, &QComboBox::activated, this, [this](int) { checkout(); });

        timer_ = new QTimer(this);
        timer_->setInterval(2500);
        connect(timer_, &QTimer::timeout, this, [this] { refresh(); });
    }

protected:
    void showEvent(QShowEvent* e) override {
        QWidget::showEvent(e);
        refresh();
        timer_->start();
    }
    void hideEvent(QHideEvent* e) override {
        timer_->stop();
        QWidget::hideEvent(e);
    }

private:
    void run(const QStringList& args) {
        const GitResult r = GitFlow::git(args);
        status_->setText(r.output.trimmed().left(300));
        refresh();
    }

    void refresh() {
        if (busy_) return;  // never repaint under a running commit
        if (!GitFlow::isRepo()) {
            status_->setText(tr("Not a git repository."));
            setEnabled(false);
            return;
        }
        setEnabled(true);

        // Do not fight the user: a list rebuild that clears their selection every
        // 2.5s makes the diff view unreadable.
        const QString keepStaged = current(staged_);
        const QString keepUnstaged = current(unstaged_);

        const QVector<Change> all = parseStatus(
            GitFlow::git({QStringLiteral("status"), QStringLiteral("--porcelain")}).output);
        staged_->clear();
        unstaged_->clear();
        for (const Change& c : all) {
            auto* item = new QListWidgetItem(
                QStringLiteral("%1  %2").arg(describeCode(c.code).leftJustified(9), c.path));
            item->setData(Qt::UserRole, c.path);
            (c.staged ? staged_ : unstaged_)->addItem(item);
        }
        reselect(staged_, keepStaged);
        reselect(unstaged_, keepUnstaged);

        commit_->setEnabled(staged_->count() > 0);
        ai_->setEnabled(staged_->count() > 0);

        const QString cur = GitFlow::branch();
        if (cur != branchName_) {
            branchName_ = cur;
            branch_->clear();
            const QString out =
                GitFlow::git({QStringLiteral("branch"), QStringLiteral("--format=%(refname:short)")})
                    .output;
            branch_->addItems(out.split(QLatin1Char('\n'), Qt::SkipEmptyParts));
            branch_->setCurrentText(cur);
        }

        log_->clear();
        const QString hist = GitFlow::git({QStringLiteral("--no-pager"), QStringLiteral("log"),
                                           QStringLiteral("--oneline"), QStringLiteral("-n"),
                                           QStringLiteral("40")})
                                 .output;
        for (const QString& l : hist.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) log_->addItem(l);
    }

    static QString current(QListWidget* w) {
        auto* i = w->currentItem();
        return i ? i->data(Qt::UserRole).toString() : QString();
    }
    static void reselect(QListWidget* w, const QString& path) {
        if (path.isEmpty()) return;
        for (int i = 0; i < w->count(); ++i)
            if (w->item(i)->data(Qt::UserRole).toString() == path) {
                w->setCurrentRow(i);
                return;
            }
    }

    void stage(QListWidgetItem* item, bool add) {
        if (!item) return;
        const QString path = item->data(Qt::UserRole).toString();
        if (add)
            GitFlow::git({QStringLiteral("add"), QStringLiteral("--"), path});
        else
            // `reset -- <path>` unstages without touching the working tree. Never
            // `checkout --` here: that would DELETE the user's edit, and an
            // "unstage" button that destroys work is a trap.
            GitFlow::git({QStringLiteral("reset"), QStringLiteral("--"), path});
        refresh();
    }

    void showDiff(QListWidgetItem* item, bool staged) {
        if (!item) return;
        const QString path = item->data(Qt::UserRole).toString();
        QStringList args{QStringLiteral("--no-pager"), QStringLiteral("diff")};
        if (staged) args << QStringLiteral("--cached");
        args << QStringLiteral("--") << path;
        QString text = GitFlow::git(args).output;
        // An untracked file has no diff at all — show its contents, which is what
        // you actually want to review before adding it.
        if (text.trimmed().isEmpty())
            text = GitFlow::git({QStringLiteral("--no-pager"), QStringLiteral("diff"),
                                 QStringLiteral("--no-index"), QStringLiteral("/dev/null"), path})
                       .output;
        paintDiff(text);
    }

    void paintDiff(const QString& text) {
        const Theme::Colors c = Theme::currentColors();
        QString html = QStringLiteral("<pre style='font-family:monospace;margin:0'>");
        for (const QString& line : text.split(QLatin1Char('\n'))) {
            QString colour = c.dim.name();
            if (line.startsWith(QLatin1Char('+')) && !line.startsWith(QStringLiteral("+++")))
                colour = c.ok.name();
            else if (line.startsWith(QLatin1Char('-')) && !line.startsWith(QStringLiteral("---")))
                colour = c.err.name();
            else if (line.startsWith(QStringLiteral("@@")))
                colour = c.accent.name();
            else
                colour = c.fg.name();
            html += QStringLiteral("<span style='color:%1'>%2</span>\n")
                        .arg(colour, line.toHtmlEscaped());
        }
        diff_->setHtml(html + QStringLiteral("</pre>"));
    }

    void checkout() {
        const QString want = branch_->currentText().trimmed();
        if (want.isEmpty() || want == branchName_) return;
        const GitResult r = GitFlow::git({QStringLiteral("checkout"), want});
        if (!r.ok()) status_->setText(r.output.trimmed().left(300));
        branchName_.clear();  // force the branch list to rebuild
        refresh();
    }

    void writeMessage() {
        const QString diff = GitFlow::stagedDiff();
        if (diff.trimmed().isEmpty()) {
            status_->setText(tr("Nothing staged."));
            return;
        }
        setBusy(true, tr("asking %1…").arg(GitFlow::modelFor(host_.currentModel())));

        QPointer<GitWidget> self = this;
        const QString backend = host_.currentBackend();
        const QString model = host_.currentModel();
        std::thread([self, diff, backend, model] {
            CancelToken cancel;
            const QString msg = GitFlow::commitMessage(diff, backend, GitFlow::modelFor(model),
                                                       cancel);
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

        // THE GATE. Same one the CLI enforces: a high-severity credential in the
        // STAGED diff stops the commit. A desktop button that quietly skipped a gate
        // the CLI applies would be worse than having no button at all.
        const QVector<Finding> bad = GitFlow::highFindings(GitFlow::stagedDiff());
        if (!bad.isEmpty()) {
            QStringList lines;
            for (const Finding& f : bad)
                lines << QStringLiteral("• %1 — %2").arg(f.rule, f.redacted);
            QMessageBox box(host_.window());
            box.setIcon(QMessageBox::Warning);
            box.setWindowTitle(tr("A secret is staged"));
            box.setText(tr("The staged diff looks like it contains a credential:"));
            box.setInformativeText(lines.join(QLatin1Char('\n')) +
                                   tr("\n\nCommit anyway? A pushed secret must be treated as "
                                      "leaked — rotate it, do not just amend it away."));
            box.setStandardButtons(QMessageBox::Cancel);
            box.addButton(tr("Commit anyway"), QMessageBox::DestructiveRole);
            box.setDefaultButton(QMessageBox::Cancel);
            if (box.exec() == QMessageBox::Cancel) {
                status_->setText(tr("Blocked: a secret is staged."));
                return;
            }
        }

        const GitResult r = GitFlow::git({QStringLiteral("commit"), QStringLiteral("-F"),
                                          QStringLiteral("-")},
                                         msg);
        if (!r.ok()) {
            status_->setText(r.output.trimmed().left(300));
            return;
        }
        message_->clear();
        status_->setText(tr("Committed."));
        refresh();
    }

    void doPush() {
        // Pushing is the first step that leaves the machine, and it is the one git
        // action that cannot be taken back. Ask, always.
        const auto go = QMessageBox::question(
            host_.window(), tr("Push"),
            tr("Push %1 to its remote?\n\nThis leaves your machine.").arg(branchName_),
            QMessageBox::Cancel | QMessageBox::Ok, QMessageBox::Cancel);
        if (go != QMessageBox::Ok) return;
        run({QStringLiteral("push")});
    }

    void setBusy(bool on, const QString& msg) {
        busy_ = on;
        ai_->setEnabled(!on && staged_->count() > 0);
        commit_->setEnabled(!on && staged_->count() > 0);
        status_->setText(msg);
    }

    PaneHost& host_;
    QComboBox* branch_;
    QListWidget* staged_;
    QListWidget* unstaged_;
    QListWidget* log_;
    QTextEdit* diff_;
    QPlainTextEdit* message_;
    QPushButton* ai_;
    QPushButton* commit_;
    QPushButton* push_;
    QLabel* status_;
    QTimer* timer_;
    QString branchName_;
    bool busy_ = false;
};

}  // namespace

PaneSpec makeGitPaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("git");
    s.title = QStringLiteral("🌿 Git");
    s.group = QStringLiteral("Tools");
    s.singleton = true;
    s.factory = [](PaneHost& h) -> QWidget* { return new GitWidget(h); };
    return s;
}

}  // namespace odv
