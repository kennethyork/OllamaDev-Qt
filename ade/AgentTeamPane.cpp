#include "AgentTeamPane.h"

#include <QCheckBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "Backend.h"
#include "Theme.h"

namespace odv {
namespace {

QString shq(const QString& s) {
    QString out = s;
    out.replace(QLatin1String("'"), QLatin1String("'\\''"));
    return QLatin1Char('\'') + out + QLatin1Char('\'');
}

// Fan one prompt out to several providers at once: pick the backends, type a
// prompt, Launch — and each selected provider gets its own terminal running
// `ollamadev --backend <id> "<prompt>"`. This is the "agent team" from the PHP
// app: parallel CLIs racing the same task, side by side on the canvas.
class AgentTeamWidget : public QWidget {
public:
    explicit AgentTeamWidget(PaneHost& host, QWidget* parent = nullptr)
        : QWidget(parent), host_(host) {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(10, 10, 10, 10);
        root->setSpacing(8);

        root->addWidget(new QLabel(tr("<b>Providers</b> — launch one terminal per pick"), this));

        auto* grid = new QHBoxLayout;
        grid->setSpacing(10);
        const QStringList available = Backends::availableIds();
        // Show every backend we know; enable only the installed ones so the team is
        // honest about what will actually spawn.
        for (const QString& id : Backends::all()) {
            const bool ok = available.contains(id);
            auto* cb = new QCheckBox(Backends::labelFor(id), this);
            cb->setEnabled(ok);
            cb->setChecked(ok);
            if (!ok) cb->setToolTip(tr("not installed on this machine"));
            cb->setProperty("backendId", id);
            boxes_.push_back(cb);
            grid->addWidget(cb);
        }
        grid->addStretch(1);
        root->addLayout(grid);

        if (available.isEmpty())
            root->addWidget(new QLabel(
                tr("<span style='color:%1'>No providers installed — install Ollama or a coding "
                   "CLI to launch a team.</span>")
                    .arg(Theme::currentColors().warn.name()),
                this));

        prompt_ = new QPlainTextEdit(this);
        prompt_->setPlaceholderText(tr("Prompt for the whole team — e.g. \"add unit tests for the "
                                       "parser and report coverage\"  (Ctrl+Enter to launch)"));
        prompt_->setMinimumHeight(90);
        root->addWidget(prompt_, 1);

        auto* bar = new QHBoxLayout;
        auto* launch = new QPushButton(tr("🚀 Launch team"), this);
        launch->setProperty("cta", true);
        status_ = new QLabel(this);
        status_->setStyleSheet(QStringLiteral("color:%1;").arg(Theme::currentColors().faint.name()));
        bar->addWidget(launch);
        bar->addWidget(status_, 1);
        root->addLayout(bar);

        connect(launch, &QPushButton::clicked, this, [this] { doLaunch(); });

        // Ctrl+Enter in the prompt launches, matching the PHP AgentTeam shortcut.
        prompt_->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == prompt_ && e->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(e);
            if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) &&
                (ke->modifiers() & Qt::ControlModifier)) {
                doLaunch();
                return true;
            }
        }
        return QWidget::eventFilter(o, e);
    }

private:
    void doLaunch() {
        const QString prompt = prompt_->toPlainText().trimmed();
        if (prompt.isEmpty()) {
            status_->setText(tr("type a prompt first"));
            prompt_->setFocus();
            return;
        }
        QStringList launched;
        for (QCheckBox* cb : boxes_) {
            if (!cb->isChecked() || !cb->isEnabled()) continue;
            const QString id = cb->property("backendId").toString();
            host_.runInTerminal(QStringLiteral("ollamadev --backend %1 %2").arg(id, shq(prompt)));
            launched << Backends::labelFor(id);
        }
        if (launched.isEmpty()) {
            status_->setText(tr("select at least one provider"));
            return;
        }
        status_->setText(tr("launched %1: %2").arg(launched.size()).arg(launched.join(", ")));
        host_.setStatus(tr("agent team launched — %1 terminals").arg(launched.size()));
    }

    PaneHost& host_;
    QVector<QCheckBox*> boxes_;
    QPlainTextEdit* prompt_ = nullptr;
    QLabel* status_ = nullptr;
};

}  // namespace

PaneSpec makeAgentTeamPaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("agentteam");
    s.title = QStringLiteral("Agent team");
    s.group = QStringLiteral("Crew");
    s.singleton = true;
    s.factory = [](PaneHost& host) -> QWidget* { return new AgentTeamWidget(host); };
    return s;
}

}  // namespace odv
