#include "CoderPane.h"

#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

#include "Crew.h"
#include "Theme.h"

namespace odv {
namespace {

// What a coder is doing, read off the tail of its own log.
//
// The log is the coder's raw output PLUS a line per tool call, which the crew
// writes as "→ edit src/Parser.cpp" (Crew.cpp, sink.onTool). So the LAST arrow
// line in the log is, by construction, the thing the coder is doing right now.
// That is the whole trick: no extra IPC, no event bus — the file the crew was
// already writing is the live feed.
struct Activity {
    QString label;   // "editing", "running", …
    QString detail;  // the file / command / pattern
};

Activity readActivity(const QString& log) {
    Activity a{QStringLiteral("thinking"), {}};
    if (log.isEmpty()) return a;

    // Last "→ tool detail" line wins.
    const int arrow = log.lastIndexOf(QStringLiteral("\n→ "));
    if (arrow < 0) return a;
    int end = log.indexOf(QLatin1Char('\n'), arrow + 1);
    if (end < 0) end = log.size();
    const QString line = log.mid(arrow + 3, end - arrow - 3).trimmed();
    if (line.isEmpty()) return a;

    const int sp = line.indexOf(QLatin1Char(' '));
    const QString tool = sp < 0 ? line : line.left(sp);
    a.detail = sp < 0 ? QString() : line.mid(sp + 1);

    // Grouped by what the user cares about — "is it writing, or just reading?" —
    // not by tool name. A coder that has been reading for two minutes is a
    // different situation from one that is editing.
    static const QHash<QString, QString> kinds{
        {QStringLiteral("edit"), QStringLiteral("editing")},
        {QStringLiteral("multi_edit"), QStringLiteral("editing")},
        {QStringLiteral("write"), QStringLiteral("writing")},
        {QStringLiteral("patch"), QStringLiteral("patching")},
        {QStringLiteral("view"), QStringLiteral("reading")},
        {QStringLiteral("ls"), QStringLiteral("looking around")},
        {QStringLiteral("grep"), QStringLiteral("searching")},
        {QStringLiteral("glob"), QStringLiteral("searching")},
        {QStringLiteral("code_search"), QStringLiteral("searching")},
        {QStringLiteral("bash"), QStringLiteral("running")},
        {QStringLiteral("bg"), QStringLiteral("running")},
        {QStringLiteral("run_tests"), QStringLiteral("testing")},
        {QStringLiteral("skill"), QStringLiteral("loading a skill")},
    };
    a.label = kinds.value(tool, tool);  // an unknown tool is described by its own name
    return a;
}

QColor stateColor(const QString& state, const Theme::Colors& c) {
    if (state == QLatin1String("doing")) return c.accent;
    if (state == QLatin1String("done")) return c.ok;
    if (state == QLatin1String("held") || state == QLatin1String("flagged")) return c.warn;
    return c.faint;  // todo
}

// One coder's tile: who it is, what it is doing, its live output, and a box to
// talk to it.
class CoderTile : public QWidget {
public:
    CoderTile(int n, QWidget* parent = nullptr) : QWidget(parent), n_(n) {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(8, 6, 8, 8);
        v->setSpacing(4);

        title_ = new QLabel(this);
        title_->setTextFormat(Qt::PlainText);
        v->addWidget(title_);

        activity_ = new QLabel(this);
        activity_->setTextFormat(Qt::PlainText);
        activity_->setWordWrap(true);
        v->addWidget(activity_);

        body_ = new QPlainTextEdit(this);
        body_->setReadOnly(true);
        body_->setMinimumHeight(120);
        body_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        body_->setFrameShape(QFrame::NoFrame);
        v->addWidget(body_, 1);

        auto* row = new QHBoxLayout;
        row->setSpacing(4);
        steer_ = new QLineEdit(this);
        steer_->setPlaceholderText(tr("say something to this coder…"));
        auto* send = new QPushButton(tr("Steer"), this);
        row->addWidget(steer_, 1);
        row->addWidget(send);
        v->addLayout(row);

        connect(send, &QPushButton::clicked, this, [this] { sendSteer(); });
        connect(steer_, &QLineEdit::returnPressed, this, [this] { sendSteer(); });
    }

    // Append only the new bytes, and keep the view pinned to the bottom unless the
    // user has scrolled up to read something — yanking the scrollbar out from under
    // someone mid-read is the fastest way to make a live pane useless.
    void appendLog(const QString& chunk) {
        if (chunk.isEmpty()) return;
        log_ += chunk;
        auto* bar = body_->verticalScrollBar();
        const bool atBottom = bar->value() >= bar->maximum() - 4;
        body_->moveCursor(QTextCursor::End);
        body_->insertPlainText(chunk);
        if (atBottom) bar->setValue(bar->maximum());
    }

    void setSubtask(const QJsonObject& s) {
        const Theme::Colors c = Theme::currentColors();
        const QString state = s.value(QStringLiteral("state")).toString();
        const QString model = s.value(QStringLiteral("model")).toString();
        const QString route = s.value(QStringLiteral("route")).toString();

        title_->setText(QStringLiteral("#%1  %2").arg(n_).arg(
            s.value(QStringLiteral("title")).toString()));
        title_->setStyleSheet(QStringLiteral("font-weight:600;color:%1")
                                  .arg(stateColor(state, c).name()));

        const Activity a = readActivity(log_);
        // A finished coder is not "thinking" — its last logged action is history.
        const QString what = state == QLatin1String("doing")
                                 ? QStringLiteral("%1 %2").arg(a.label, a.detail)
                                 : QStringLiteral("· %1").arg(state);
        activity_->setText(
            QStringLiteral("%1\n%2%3").arg(what.trimmed(), model,
                                           route.isEmpty() ? QString()
                                                           : QStringLiteral(" · %1").arg(route)));
        activity_->setStyleSheet(QStringLiteral("color:%1").arg(c.dim.name()));

        // Steering a coder that has already finished would go nowhere: the loop it
        // would have been injected into has exited.
        const bool live = state == QLatin1String("doing") || state == QLatin1String("todo");
        steer_->setEnabled(live);
        steer_->setPlaceholderText(live ? tr("say something to this coder…")
                                        : tr("this coder has finished"));

        setStyleSheet(QStringLiteral("QWidget{background:%1;border:1px solid %2;border-radius:6px}")
                          .arg(c.elev.name(), stateColor(state, c).name()));
    }

    qint64 offset = 0;  // how much of the log we have already pulled

private:
    void sendSteer() {
        const QString msg = steer_->text().trimmed();
        if (msg.isEmpty()) return;
        QString err;
        if (Crew::steer(n_, msg, &err)) {
            steer_->clear();
            // Echo it locally: the coder only picks it up at its next iteration, and
            // silence in between reads as "the button did nothing".
            appendLog(QStringLiteral("\n[you said: %1]\n").arg(msg));
        } else {
            appendLog(QStringLiteral("\n[could not steer: %1]\n").arg(err));
        }
    }

    int n_;
    QString log_;
    QLabel* title_;
    QLabel* activity_;
    QPlainTextEdit* body_;
    QLineEdit* steer_;
};

// The pane: a grid of tiles, one per coder, rebuilt whenever the run changes.
class CoderPaneView : public QWidget {
public:
    explicit CoderPaneView(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(360, 260);
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);

        empty_ = new QLabel(tr("No crew running.\n\nStart one and each coder appears here, live."),
                            this);
        empty_->setAlignment(Qt::AlignCenter);
        v->addWidget(empty_);

        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        host_ = new QWidget(scroll);
        grid_ = new QGridLayout(host_);
        grid_->setContentsMargins(6, 6, 6, 6);
        grid_->setSpacing(6);
        scroll->setWidget(host_);
        v->addWidget(scroll, 1);
        scroll_ = scroll;
        scroll_->hide();

        timer_ = new QTimer(this);
        timer_->setInterval(700);  // the crew writes continuously; this feels live
        connect(timer_, &QTimer::timeout, this, [this] { poll(); });
    }

protected:
    void showEvent(QShowEvent* e) override {
        QWidget::showEvent(e);
        poll();
        timer_->start();
    }
    void hideEvent(QHideEvent* e) override {
        timer_->stop();
        QWidget::hideEvent(e);
    }

private:
    void poll() {
        const QJsonObject board = Crew::boardState();
        const QString runId = board.value(QStringLiteral("runId")).toString();
        const QJsonArray subs = board.value(QStringLiteral("subtasks")).toArray();

        if (runId.isEmpty() || subs.isEmpty()) {
            empty_->show();
            scroll_->hide();
            return;
        }
        empty_->hide();
        scroll_->show();

        // A new run (or a re-plan that added coders) rebuilds the grid. Tiles are
        // keyed by coder NUMBER, not by index: a --replan renumbers new coders past
        // the kept ones, so #1 and #3 can coexist with no #2.
        if (runId != runId_ || subs.size() != tiles_.size()) {
            runId_ = runId;
            qDeleteAll(tiles_);
            tiles_.clear();
            for (int i = 0; i < subs.size(); ++i) {
                const int n = subs.at(i).toObject().value(QStringLiteral("n")).toInt();
                auto* tile = new CoderTile(n, host_);
                tiles_.insert(n, tile);
                grid_->addWidget(tile, i / 2, i % 2);
            }
        }

        for (const QJsonValue& v : subs) {
            const QJsonObject s = v.toObject();
            const int n = s.value(QStringLiteral("n")).toInt();
            CoderTile* tile = tiles_.value(n);
            if (!tile) continue;

            // Only ever pull the DELTA. These logs run to hundreds of KB on a long
            // run, and re-reading the whole file twice a second would make the pane
            // the most expensive thing on the machine.
            qint64 size = 0;
            const QString fresh = Crew::coderLog(runId, n, tile->offset, &size);
            tile->offset = size;
            tile->appendLog(fresh);
            tile->setSubtask(s);
        }
    }

    QString runId_;
    QHash<int, CoderTile*> tiles_;
    QLabel* empty_;
    QScrollArea* scroll_;
    QWidget* host_;
    QGridLayout* grid_;
    QTimer* timer_;
};

}  // namespace

PaneSpec makeCoderPaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("coders");
    s.title = QStringLiteral("Coders");
    s.group = QStringLiteral("Crew");
    s.singleton = true;
    s.factory = [](PaneHost&) -> QWidget* { return new CoderPaneView; };
    return s;
}

}  // namespace odv
