#include "StartPane.h"

#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "Theme.h"

namespace odv {
namespace {

// The Start page: one-click tiles for the health/onboarding commands (setup,
// doctor, verify) plus quick launches for the everyday flows, each opened in a
// terminal via the host.
//
// Simplified vs the PHP Start view, which also had "open this pane" shortcuts:
// PaneHost has no open-a-pane call (panes reach the app only through project/
// status/openFile/runInTerminal), so instead of dead buttons the quick tiles run
// the CLI equivalents — which is what those panes drive anyway.
class StartWidget : public QWidget {
public:
    explicit StartWidget(PaneHost& host, QWidget* parent = nullptr)
        : QWidget(parent), host_(host) {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(16, 16, 16, 16);
        root->setSpacing(14);

        auto* title = new QLabel(tr("<h2>OllamaDev</h2>"), this);
        root->addWidget(title);

        root->addWidget(section(tr("Get set up")));
        auto* setup = new QGridLayout;
        setup->setSpacing(10);
        addTile(setup, 0, 0, tr("Doctor"),
                tr("Check Ollama, models, and CLIs are healthy"),
                QStringLiteral("ollamadev doctor"));
        addTile(setup, 0, 1, tr("Setup"),
                tr("First-run setup — pick models and providers"),
                QStringLiteral("ollamadev setup"));
        addTile(setup, 0, 2, tr("Verify"),
                tr("Run the project's tests and auto-fix failures"),
                QStringLiteral("ollamadev verify"));
        root->addLayout(setup);

        root->addWidget(section(tr("Jump in")));
        auto* quick = new QGridLayout;
        quick->setSpacing(10);
        addTile(quick, 0, 0, tr("Chat"),
                tr("Interactive agent for this folder"), QStringLiteral("ollamadev"));
        addTile(quick, 0, 1, tr("Crew"),
                tr("Plan → parallel coders → audit → land"),
                QStringLiteral("ollamadev crew"));
        addTile(quick, 0, 2, tr("Index"),
                tr("Build the semantic code index"),
                QStringLiteral("ollamadev index build"));
        addTile(quick, 1, 0, tr("Models"),
                tr("List models on the active backend"),
                QStringLiteral("ollamadev models"));
        addTile(quick, 1, 1, tr("Memory"),
                tr("List the wiki-linked notes"),
                QStringLiteral("ollamadev memory list"));
        addTile(quick, 1, 2, tr("Ship"),
                tr("Stage → scan → AI commit → push"),
                QStringLiteral("ollamadev ship"));
        root->addLayout(quick);

        root->addStretch(1);
        auto* hint = new QLabel(
            tr("<span style='color:%1'>Each tile opens a terminal running the command. Add panes "
               "from the ＋ menu.</span>")
                .arg(Theme::currentColors().faint.name()),
            this);
        hint->setWordWrap(true);
        root->addWidget(hint);
    }

private:
    QLabel* section(const QString& text) {
        auto* l = new QLabel(QStringLiteral("<b>%1</b>").arg(text.toHtmlEscaped()), this);
        l->setStyleSheet(QStringLiteral("color:%1;").arg(Theme::currentColors().dim.name()));
        return l;
    }

    void addTile(QGridLayout* grid, int r, int cIdx, const QString& title, const QString& desc,
                 const QString& cmd) {
        const Theme::Colors c = Theme::currentColors();
        auto* b = new QPushButton(this);
        b->setText(QStringLiteral("%1\n%2").arg(title, desc));
        b->setMinimumHeight(72);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(
            QStringLiteral("QPushButton{text-align:left;padding:10px 12px;background:%1;"
                           "border:1px solid %2;border-radius:8px;color:%3;}"
                           "QPushButton:hover{border-color:%4;background:%5;}")
                .arg(c.bg2.name(), c.border.name(), c.fg.name(), c.accent.name(), c.bg3.name()));
        connect(b, &QPushButton::clicked, this, [this, cmd, title] {
            host_.runInTerminal(cmd);
            host_.setStatus(tr("▶ %1").arg(title));
        });
        grid->addWidget(b, r, cIdx);
    }

    PaneHost& host_;
};

}  // namespace

PaneSpec makeStartPaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("start");
    s.title = QStringLiteral("Start");
    s.group = QStringLiteral("Views");
    s.singleton = true;
    s.factory = [](PaneHost& host) -> QWidget* { return new StartWidget(host); };
    return s;
}

}  // namespace odv
