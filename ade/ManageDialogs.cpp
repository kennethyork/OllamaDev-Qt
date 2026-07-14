// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ManageDialogs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTextEdit>
#include <QVBoxLayout>

#include "GitFlow.h"
#include "Hooks.h"
#include "Skills.h"
#include "Theme.h"

namespace odv {
namespace ManageDialogs {
namespace {

// A frameless-ish modeless dialog reads wrong for these — they are modal editors,
// so a real QDialog with the window as parent inherits the palette and centres
// itself. Shared setup so the five below look like one family.
QDialog* makeDialog(PaneHost& host, const QString& title, int w, int h) {
    auto* dlg = new QDialog(host.window());
    dlg->setWindowTitle(title);
    dlg->setModal(true);
    dlg->resize(w, h);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    return dlg;
}

QListWidget* makeList() {
    auto* l = new QListWidget;
    l->setAlternatingRowColors(true);
    l->setWordWrap(true);
    return l;
}

}  // namespace

// ---- roles -----------------------------------------------------------------

void openRoles(PaneHost& host) {
    QDialog* dlg = makeDialog(host, QObject::tr("Crew Roles"), 560, 620);
    auto* v = new QVBoxLayout(dlg);

    v->addWidget(new QLabel(QObject::tr("Personas the Director assigns per subtask. "
                                        "Built-ins are read-only; your roles can be removed.")));
    QListWidget* list = makeList();
    v->addWidget(list, 1);

    auto reload = [list] {
        list->clear();
        for (const CrewRole& r : CrewRoles::all()) {
            QStringList badges;
            if (!r.custom) badges << QObject::tr("built-in");
            if (!r.model.isEmpty()) badges << r.model;
            if (r.readOnly) badges << QObject::tr("read-only");
            const QString tag = badges.isEmpty() ? QString()
                                                 : QStringLiteral("  [%1]").arg(badges.join(", "));
            auto* it = new QListWidgetItem(
                r.name + tag + QStringLiteral("\n") +
                    (r.desc.isEmpty() ? QObject::tr("no description") : r.desc),
                list);
            it->setData(Qt::UserRole, r.name);
            it->setData(Qt::UserRole + 1, r.custom);  // only custom roles are removable
        }
    };
    reload();

    // Add form.
    auto* form = new QFormLayout;
    auto* name = new QLineEdit;
    auto* persona = new QPlainTextEdit;
    persona->setPlaceholderText(QObject::tr("The persona injected into the coder's system prompt"));
    persona->setMaximumHeight(90);
    auto* desc = new QLineEdit;
    auto* model = new QLineEdit;
    model->setPlaceholderText(QObject::tr("optional — pin a model (blank = crew coder model)"));
    auto* readOnly = new QCheckBox(QObject::tr("read-only role"));
    form->addRow(QObject::tr("Name"), name);
    form->addRow(QObject::tr("Persona"), persona);
    form->addRow(QObject::tr("Description"), desc);
    form->addRow(QObject::tr("Model"), model);
    form->addRow(QString(), readOnly);
    v->addLayout(form);

    auto* row = new QHBoxLayout;
    auto* remove = new QPushButton(QObject::tr("Remove selected"));
    remove->setProperty("danger", true);
    auto* save = new QPushButton(QObject::tr("Save role"));
    save->setProperty("cta", true);
    row->addWidget(remove);
    row->addStretch(1);
    row->addWidget(save);
    v->addLayout(row);

    QObject::connect(save, &QPushButton::clicked, dlg, [=, &host] {
        const QString n = name->text().trimmed();
        const QString p = persona->toPlainText().trimmed();
        if (n.isEmpty() || p.isEmpty()) {
            host.setStatus(QObject::tr("a role needs a name and a persona"));
            return;
        }
        CrewRoles::add(n, p, desc->text().trimmed(), model->text().trimmed(), readOnly->isChecked());
        name->clear();
        persona->clear();
        desc->clear();
        model->clear();
        readOnly->setChecked(false);
        reload();
        host.setStatus(QObject::tr("role \"%1\" saved").arg(n));
    });
    QObject::connect(remove, &QPushButton::clicked, dlg, [=, &host] {
        QListWidgetItem* it = list->currentItem();
        if (!it) return;
        if (!it->data(Qt::UserRole + 1).toBool()) {
            host.setStatus(QObject::tr("built-in roles cannot be removed"));
            return;
        }
        const QString n = it->data(Qt::UserRole).toString();
        CrewRoles::remove(n);
        reload();
        host.setStatus(QObject::tr("role \"%1\" removed").arg(n));
    });

    dlg->show();
}

// ---- skills ----------------------------------------------------------------

void openSkills(PaneHost& host) {
    QDialog* dlg = makeDialog(host, QObject::tr("Skills"), 620, 680);
    auto* v = new QVBoxLayout(dlg);

    v->addWidget(new QLabel(QObject::tr("Reusable instructions the agent/crew load on demand. "
                                        "Select one to edit; saving a built-in creates your "
                                        "editable override.")));
    QListWidget* list = makeList();
    v->addWidget(list, 1);

    auto* form = new QFormLayout;
    auto* name = new QLineEdit;
    auto* desc = new QLineEdit;
    auto* body = new QPlainTextEdit;
    body->setPlaceholderText(QObject::tr("The skill's instructions (SKILL.md body)"));
    form->addRow(QObject::tr("Name"), name);
    form->addRow(QObject::tr("Description"), desc);
    form->addRow(QObject::tr("Instructions"), body);
    v->addLayout(form, 1);

    auto reload = [list] {
        list->clear();
        for (const Skill& s : Skills::listForManager()) {
            const QString tag = s.builtin ? QObject::tr("  [built-in]") : QString();
            auto* it = new QListWidgetItem(
                s.name + tag + QStringLiteral("\n") +
                    (s.description.isEmpty() ? QObject::tr("no description") : s.description),
                list);
            it->setData(Qt::UserRole, s.name);
        }
    };
    reload();

    // Clicking a row loads its full body into the form.
    QObject::connect(list, &QListWidget::currentItemChanged, dlg,
                     [=](QListWidgetItem* it, QListWidgetItem*) {
                         if (!it) return;
                         const Skill s = Skills::get(it->data(Qt::UserRole).toString());
                         if (s.isNull()) return;
                         name->setText(s.name);
                         desc->setText(s.description);
                         body->setPlainText(s.body);
                     });

    auto* row = new QHBoxLayout;
    auto* newBtn = new QPushButton(QObject::tr("New"));
    auto* remove = new QPushButton(QObject::tr("Remove selected"));
    remove->setProperty("danger", true);
    auto* save = new QPushButton(QObject::tr("Save skill"));
    save->setProperty("cta", true);
    row->addWidget(newBtn);
    row->addWidget(remove);
    row->addStretch(1);
    row->addWidget(save);
    v->addLayout(row);

    QObject::connect(newBtn, &QPushButton::clicked, dlg, [=] {
        list->setCurrentItem(nullptr);
        name->clear();
        desc->clear();
        body->clear();
        name->setFocus();
    });
    QObject::connect(save, &QPushButton::clicked, dlg, [=, &host] {
        const QString n = name->text().trimmed();
        const QString b = body->toPlainText().trimmed();
        if (n.isEmpty() || b.isEmpty()) {
            host.setStatus(QObject::tr("a skill needs a name and instructions"));
            return;
        }
        const QString slug = Skills::save(n, desc->text().trimmed(), b);
        if (slug.isEmpty()) {
            host.setStatus(QObject::tr("could not save skill"));
            return;
        }
        reload();
        host.setStatus(QObject::tr("skill \"%1\" saved").arg(n));
    });
    QObject::connect(remove, &QPushButton::clicked, dlg, [=, &host] {
        QListWidgetItem* it = list->currentItem();
        if (!it) return;
        const QString n = it->data(Qt::UserRole).toString();
        if (!Skills::remove(n)) {
            host.setStatus(QObject::tr("built-in skills have no copy to remove"));
            return;
        }
        name->clear();
        desc->clear();
        body->clear();
        reload();
        host.setStatus(QObject::tr("skill \"%1\" removed").arg(n));
    });

    dlg->show();
}

// ---- hooks -----------------------------------------------------------------

void openHooks(PaneHost& host) {
    QDialog* dlg = makeDialog(host, QObject::tr("Hooks"), 600, 560);
    auto* v = new QVBoxLayout(dlg);

    v->addWidget(new QLabel(QObject::tr("Shell commands fired at lifecycle points. A PreToolUse "
                                        "hook exiting non-zero blocks the tool.")));
    QListWidget* list = makeList();
    v->addWidget(list, 1);

    auto reload = [list] {
        list->clear();
        for (const QString& ev : Hooks::knownEvents()) {
            const QVector<Hooks::Hook> hooks = Hooks::listFor(ev);
            for (int i = 0; i < hooks.size(); ++i) {
                const Hooks::Hook& h = hooks[i];
                const QString mx =
                    h.matcher.isEmpty() ? QString()
                                        : QStringLiteral("  match: %1").arg(h.matcher);
                auto* it = new QListWidgetItem(
                    ev + mx + QStringLiteral("\n") + h.command, list);
                it->setData(Qt::UserRole, ev);
                it->setData(Qt::UserRole + 1, i);
            }
        }
        if (list->count() == 0)
            new QListWidgetItem(QObject::tr("No hooks configured."), list);
    };

    auto* form = new QFormLayout;
    auto* event = new QComboBox;
    event->addItems(Hooks::knownEvents());
    auto* command = new QLineEdit;
    command->setPlaceholderText(QObject::tr("shell command to run"));
    auto* matcher = new QLineEdit;
    matcher->setPlaceholderText(QObject::tr("optional regex on the tool name"));
    form->addRow(QObject::tr("Event"), event);
    form->addRow(QObject::tr("Command"), command);
    form->addRow(QObject::tr("Matcher"), matcher);
    v->addLayout(form);

    auto* row = new QHBoxLayout;
    auto* remove = new QPushButton(QObject::tr("Remove selected"));
    remove->setProperty("danger", true);
    auto* add = new QPushButton(QObject::tr("Add hook"));
    add->setProperty("cta", true);
    row->addWidget(remove);
    row->addStretch(1);
    row->addWidget(add);
    v->addLayout(row);

    reload();

    QObject::connect(add, &QPushButton::clicked, dlg, [=, &host] {
        const QString cmd = command->text().trimmed();
        if (cmd.isEmpty()) {
            host.setStatus(QObject::tr("a hook needs a command"));
            return;
        }
        if (!Hooks::add(event->currentText(), cmd, matcher->text().trimmed())) {
            host.setStatus(QObject::tr("could not add hook"));
            return;
        }
        command->clear();
        matcher->clear();
        reload();
        host.setStatus(QObject::tr("%1 hook added").arg(event->currentText()));
    });
    QObject::connect(remove, &QPushButton::clicked, dlg, [=, &host] {
        QListWidgetItem* it = list->currentItem();
        if (!it) return;
        const QString ev = it->data(Qt::UserRole).toString();
        if (ev.isEmpty()) return;  // the "no hooks" placeholder
        const int idx = it->data(Qt::UserRole + 1).toInt();
        Hooks::removeAt(ev, idx);
        reload();
        host.setStatus(QObject::tr("hook removed"));
    });

    dlg->show();
}

// ---- crew launch -----------------------------------------------------------

void openCrewLaunch(PaneHost& host) {
    QDialog* dlg = makeDialog(host, QObject::tr("Launch Crew"), 560, 480);
    auto* v = new QVBoxLayout(dlg);

    v->addWidget(new QLabel(QObject::tr("Plan a multi-agent run. This builds an "
                                        "\"ollamadev crew\" command and runs it in a new terminal.")));

    auto* task = new QPlainTextEdit;
    task->setPlaceholderText(QObject::tr("What should the crew do? e.g. \"Add dark-mode toggle\""));
    v->addWidget(task, 1);

    auto* form = new QFormLayout;
    auto* max = new QSpinBox;
    max->setRange(1, 8);
    max->setValue(4);
    auto* focus = new QLineEdit;
    focus->setPlaceholderText(QObject::tr("optional — stack/context for the agents to follow"));
    auto* review = new QCheckBox(QObject::tr("Hold work for review instead of auto-landing"));
    auto* research = new QCheckBox(QObject::tr("Research phase"));
    research->setChecked(true);
    auto* audit = new QCheckBox(QObject::tr("Audit phase"));
    audit->setChecked(true);
    // The brain — all opt-in, exactly as on the CLI. Off = the plain crew.
    auto* brain = new QCheckBox(QObject::tr("Use the brain — auto-pick a model per subtask by difficulty"));
    auto* debate = new QCheckBox(QObject::tr("Debate each change (advocate vs skeptic vs judge)"));
    auto* dedupe = new QCheckBox(QObject::tr("Dedupe — hold work that duplicates another coder's"));
    auto* learn = new QCheckBox(QObject::tr("Learn — remember what this run teaches for next time"));
    form->addRow(QObject::tr("Max coders"), max);
    form->addRow(QObject::tr("Focus"), focus);
    form->addRow(QString(), review);
    form->addRow(QString(), research);
    form->addRow(QString(), audit);
    form->addRow(QString(), brain);
    form->addRow(QString(), debate);
    form->addRow(QString(), dedupe);
    form->addRow(QString(), learn);
    v->addLayout(form);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto* launch = box->addButton(QObject::tr("Launch"), QDialogButtonBox::AcceptRole);
    launch->setProperty("cta", true);
    v->addWidget(box);

    QObject::connect(box, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    QObject::connect(launch, &QPushButton::clicked, dlg, [=, &host] {
        const QString t = task->toPlainText().trimmed();
        if (t.isEmpty()) {
            host.setStatus(QObject::tr("the crew needs a task"));
            return;
        }
        // The task is quoted; a stray double-quote in it would break the shell word,
        // so escape it. These flags are the ones the CLI's cmdCrew actually parses.
        QString safe = t;
        safe.replace(QLatin1Char('"'), QStringLiteral("\\\""));
        QString cmd = QStringLiteral("ollamadev crew \"%1\"").arg(safe);
        cmd += QStringLiteral(" --max %1").arg(max->value());
        if (review->isChecked()) cmd += QStringLiteral(" --review");
        if (!research->isChecked()) cmd += QStringLiteral(" --no-research");
        if (!audit->isChecked()) cmd += QStringLiteral(" --no-audit");
        if (brain->isChecked()) cmd += QStringLiteral(" --route");
        if (debate->isChecked()) cmd += QStringLiteral(" --debate");
        if (dedupe->isChecked()) cmd += QStringLiteral(" --dedupe");
        if (learn->isChecked()) cmd += QStringLiteral(" --learn");
        const QString f = focus->text().trimmed();
        if (!f.isEmpty()) {
            QString sf = f;
            sf.replace(QLatin1Char('"'), QStringLiteral("\\\""));
            cmd += QStringLiteral(" --focus \"%1\"").arg(sf);
        }
        host.runInTerminal(cmd);
        dlg->accept();
    });

    dlg->show();
}

// ---- review / diff ---------------------------------------------------------

void openReview(PaneHost& host) {
    QDialog* dlg = makeDialog(host, QObject::tr("Review changes"), 820, 680);
    auto* v = new QVBoxLayout(dlg);

    auto* head = new QLabel;
    v->addWidget(head);

    auto* view = new QTextEdit;
    view->setReadOnly(true);
    view->setLineWrapMode(QTextEdit::NoWrap);
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    v->addWidget(view, 1);

    auto* row = new QHBoxLayout;
    auto* refresh = new QPushButton(QObject::tr("Refresh"));
    auto* close = new QPushButton(QObject::tr("Close"));
    row->addStretch(1);
    row->addWidget(refresh);
    row->addWidget(close);
    v->addLayout(row);
    QObject::connect(close, &QPushButton::clicked, dlg, &QDialog::accept);

    auto load = [=] {
        // Straight to GitFlow (the desktop links odv-core): this is exactly the data
        // `ollamadev diff --json` returns — branch + working-tree diff — without the
        // fragility of locating the CLI binary on PATH.
        if (!GitFlow::isRepo()) {
            head->setText(QObject::tr("Not a git repository."));
            view->clear();
            return;
        }
        const QString branch = GitFlow::branch();
        const QString diff = GitFlow::workingDiff();
        if (diff.trimmed().isEmpty()) {
            head->setText(QObject::tr("On %1 — no working-tree changes. Clean tree ✓").arg(branch));
            view->clear();
            return;
        }
        const Theme::Colors c = Theme::currentColors();
        auto span = [](const QString& text, const QColor& col, bool bold = false) {
            QString safe = text.toHtmlEscaped();
            if (safe.isEmpty()) safe = QStringLiteral("&nbsp;");
            return QStringLiteral("<span style=\"color:%1;%2\">%3</span>")
                .arg(col.name(), bold ? QStringLiteral("font-weight:bold;") : QString(), safe);
        };
        QString html = QStringLiteral("<pre style=\"margin:0;white-space:pre;\">");
        int adds = 0, dels = 0;
        for (const QString& line : diff.split('\n')) {
            if (line.startsWith(QLatin1String("+++")) || line.startsWith(QLatin1String("---")))
                html += span(line, c.fg, true);
            else if (line.startsWith(QLatin1String("diff --git")))
                html += span(line, c.accent, true);
            else if (line.startsWith(QLatin1String("@@")))
                html += span(line, c.accent2);
            else if (line.startsWith('+')) {
                html += span(line, c.ok);
                ++adds;
            } else if (line.startsWith('-')) {
                html += span(line, c.err);
                ++dels;
            } else html += span(line, c.dim);
            html += QLatin1Char('\n');
        }
        html += QStringLiteral("</pre>");
        view->setHtml(html);
        head->setText(QObject::tr("On %1 — +%2 / -%3").arg(branch).arg(adds).arg(dels));
    };

    QObject::connect(refresh, &QPushButton::clicked, dlg, [load] { load(); });
    load();
    dlg->show();
}

}  // namespace ManageDialogs
}  // namespace odv
