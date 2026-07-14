#include "RebaseDialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <thread>

#include "GitFlow.h"
#include "Rebase.h"
#include "Theme.h"

namespace odv {
namespace {

const char* kActions[] = {"pick", "fixup", "reword", "drop"};

QString explain(RebaseStep::Action a) {
    switch (a) {
        case RebaseStep::Fixup: return QObject::tr("folded into the commit above");
        case RebaseStep::Drop: return QObject::tr("deleted");
        case RebaseStep::Reword: return QObject::tr("kept, message rewritten");
        case RebaseStep::Pick: break;
    }
    return QObject::tr("kept as it is");
}

}  // namespace

RebaseDialog::RebaseDialog(PaneHost& host, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Tidy history"));
    resize(760, 520);

    auto* v = new QVBoxLayout(this);

    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Tidy up the last"), this));
    auto* count = new QSpinBox(this);
    count->setRange(2, 50);
    count->setValue(10);
    top->addWidget(count);
    top->addWidget(new QLabel(tr("commits"), this));
    top->addStretch(1);
    auto* propose = new QPushButton(tr("Ask the model"), this);
    propose->setProperty("cta", true);
    top->addWidget(propose);
    v->addLayout(top);

    auto* note = new QLabel(this);
    note->setWordWrap(true);
    note->setText(tr("Nothing is rewritten until you press Rebase — and a backup of where you are "
                     "now is taken first, so it is always undoable."));
    note->setStyleSheet(QStringLiteral("color:%1").arg(Theme::currentColors().dim.name()));
    v->addWidget(note);

    auto* tree = new QTreeWidget(this);
    tree->setHeaderLabels({tr("Commit"), tr("What happens to it"), tr("New message")});
    tree->setRootIsDecorated(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->setColumnWidth(1, 190);
    tree->setColumnWidth(2, 220);
    v->addWidget(tree, 1);

    auto* rationale = new QLabel(this);
    rationale->setWordWrap(true);
    rationale->setStyleSheet(QStringLiteral("color:%1").arg(Theme::currentColors().dim.name()));
    v->addWidget(rationale);

    auto* status = new QLabel(this);
    status->setWordWrap(true);
    v->addWidget(status);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto* go = buttons->addButton(tr("Rebase"), QDialogButtonBox::AcceptRole);
    go->setProperty("cta", true);
    v->addWidget(buttons);

    // The plan the dialog is currently showing. Every widget below edits THIS, and
    // apply() reads it — so what runs is exactly what is on screen.
    auto plan = std::make_shared<RebasePlan>();

    // Rebuild the table from the plan. Each row's action combo and message box write
    // straight back into the plan, so there is never a second copy to fall out of
    // step with what the user is looking at.
    const auto repaint = [tree, plan, this] {
        tree->clear();
        for (int i = 0; i < plan->steps.size(); ++i) {
            RebaseStep& s = plan->steps[i];
            auto* it = new QTreeWidgetItem(
                tree, {QStringLiteral("%1  %2").arg(s.sha.left(8), s.subject)});

            auto* combo = new QComboBox(tree);
            for (const char* a : kActions) combo->addItem(QString::fromLatin1(a));
            combo->setCurrentIndex(static_cast<int>(s.action));
            tree->setItemWidget(it, 1, combo);

            auto* msg = new QLineEdit(tree);
            msg->setText(s.newMessage);
            msg->setPlaceholderText(tr("(only used by reword)"));
            msg->setEnabled(s.action == RebaseStep::Reword);
            tree->setItemWidget(it, 2, msg);

            // The first commit has nothing above it to fold into, so `fixup` there is
            // not a choice the user should be able to make — git would reject the
            // whole todo and they would get a cryptic failure instead of a plan.
            if (i == 0) {
                auto* model = qobject_cast<QStandardItemModel*>(combo->model());
                if (model && model->item(1)) model->item(1)->setEnabled(false);
                combo->setToolTip(tr("The first commit has nothing above it to fold into."));
            }

            QObject::connect(combo, &QComboBox::currentIndexChanged, this,
                             [plan, i, msg, it](int idx) {
                                 plan->steps[i].action = static_cast<RebaseStep::Action>(idx);
                                 msg->setEnabled(plan->steps[i].action == RebaseStep::Reword);
                                 it->setText(1, QString());
                             });
            QObject::connect(msg, &QLineEdit::textChanged, this, [plan, i](const QString& t) {
                plan->steps[i].newMessage = t;
            });

            it->setToolTip(1, explain(s.action));
            if (s.action == RebaseStep::Drop) it->setForeground(0, Theme::currentColors().err);
        }
    };

    const auto load = [plan, count, repaint, status, rationale] {
        *plan = Rebase::planFor(count->value());
        rationale->clear();
        status->setText(plan->steps.isEmpty()
                            ? QObject::tr("No commits to tidy.")
                            : QObject::tr("%1 commits.").arg(plan->steps.size()));
        repaint();
    };
    load();
    connect(count, &QSpinBox::valueChanged, this, [load](int) { load(); });

    // ---- the model proposes ------------------------------------------------
    connect(propose, &QPushButton::clicked, this, [=, &host]() mutable {
        if (plan->steps.isEmpty()) return;
        propose->setEnabled(false);
        status->setText(tr("reading your commits…"));

        QPointer<RebaseDialog> self = this;
        const RebasePlan in = *plan;
        const QString backend = host.currentBackend();
        const QString model = GitFlow::modelFor(host.currentModel());
        std::thread([=]() mutable {
            CancelToken cancel;
            const RebasePlan out = Rebase::propose(in, backend, model, cancel);
            QMetaObject::invokeMethod(qApp, [=]() mutable {
                if (!self) return;
                propose->setEnabled(true);
                *plan = out;
                repaint();
                rationale->setText(out.rationale);
                int folded = 0, reworded = 0, dropped = 0;
                for (const RebaseStep& s : out.steps) {
                    if (s.action == RebaseStep::Fixup) ++folded;
                    if (s.action == RebaseStep::Reword) ++reworded;
                    if (s.action == RebaseStep::Drop) ++dropped;
                }
                status->setText(
                    tr("%1 to fold · %2 to reword · %3 to drop — check every line before you "
                       "rebase.")
                        .arg(folded)
                        .arg(reworded)
                        .arg(dropped));
            });
        }).detach();
    });

    // ---- run it ------------------------------------------------------------
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(go, &QPushButton::clicked, this, [=, &host]() mutable {
        QString err;
        const RebaseResult r = Rebase::apply(*plan, &err);
        if (r.ok) {
            QMessageBox::information(
                this, tr("Rebased"),
                tr("History rewritten.\n\nIf it is not what you wanted, undo it with:\n"
                   "    git reset --hard %1")
                    .arg(r.backupRef));
            accept();
            return;
        }
        if (r.conflicted) {
            // Do NOT silently abort: the user may want to resolve it. Offer, and say
            // exactly what each choice does.
            const auto pick = QMessageBox::question(
                this, tr("Conflict"),
                tr("The rebase stopped on a conflict.\n\n%1\n\nAbort it and put everything back the "
                   "way it was, or leave it stopped so you can resolve it yourself?")
                    .arg(r.output.left(400)),
                QMessageBox::Abort | QMessageBox::Ignore, QMessageBox::Abort);
            if (pick == QMessageBox::Abort) Rebase::abort();
            reject();
            return;
        }
        QMessageBox::warning(this, tr("Not rebased"), err.isEmpty() ? r.output : err);
    });
}

}  // namespace odv
