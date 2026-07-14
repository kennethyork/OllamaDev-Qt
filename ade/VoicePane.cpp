#include "VoicePane.h"

#include <QApplication>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QVBoxLayout>

#include <thread>

#include "Crew.h"
#include "Stt.h"
#include "Theme.h"

namespace odv {
namespace {

// Shell-quote a fragment we hand to a terminal. Single quotes with the classic
// '\'' escape: the safe way to pass an arbitrary transcription as one argv word.
QString shq(const QString& s) {
    QString out = s;
    out.replace(QLatin1String("'"), QLatin1String("'\\''"));
    return QLatin1Char('\'') + out + QLatin1Char('\'');
}

// Press-to-talk voice control, 100% local. Records with Stt::Recorder (which owns
// its recorder child by handle — no scraped PIDs), transcribes with Stt, then
// either runs the speech as a command or dictates it into a terminal.
//
// Ported command grammar (a pragmatic subset of the PHP VoiceCtl): crew steering
// ("tell coder 2 to add tests", "tell the crew to …"), crew launch ("start a crew
// to …"), and dictation ("type/say/send …" → terminal). The PHP view-opening
// commands ("open files", "show board") are intentionally dropped: PaneHost
// exposes no open-a-pane call, so faking them would be a dead end. Everything the
// host CAN do is wired.
class VoiceWidget : public QWidget {
public:
    explicit VoiceWidget(PaneHost& host, QWidget* parent = nullptr)
        : QWidget(parent), host_(host) {
        rec_ = new SttRecorder(this);  // stopped with this widget if still running

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(10, 10, 10, 10);
        root->setSpacing(8);

        ptt_ = new QPushButton(tr("Press to talk"), this);
        ptt_->setMinimumHeight(46);
        ptt_->setProperty("cta", true);
        root->addWidget(ptt_);

        auto* modes = new QHBoxLayout;
        auto* mAuto = new QRadioButton(tr("Auto"), this);
        auto* mCmd = new QRadioButton(tr("Command"), this);
        auto* mCli = new QRadioButton(tr("To terminal"), this);
        mAuto->setChecked(true);
        auto* grp = new QButtonGroup(this);
        grp->addButton(mAuto, 0);
        grp->addButton(mCmd, 1);
        grp->addButton(mCli, 2);
        modes->addWidget(new QLabel(tr("Mode:"), this));
        modes->addWidget(mAuto);
        modes->addWidget(mCmd);
        modes->addWidget(mCli);
        modes->addStretch(1);
        root->addLayout(modes);
        connect(grp, &QButtonGroup::idClicked, this, [this](int id) { mode_ = id; });

        heard_ = new QLabel(this);
        heard_->setWordWrap(true);
        heard_->setStyleSheet(QStringLiteral("font-size:15px;"));
        root->addWidget(heard_);

        log_ = new QPlainTextEdit(this);
        log_->setReadOnly(true);
        log_->setMaximumBlockCount(60);
        root->addWidget(log_, 1);

        connect(ptt_, &QPushButton::clicked, this, [this] { toggle(); });

        if (!SttRecorder::canRecord())
            note(tr("No microphone recorder found (need arecord, parecord, or ffmpeg)."), true);
        else if (!Stt::available())
            note(tr("No local speech engine yet — run `ollamadev voice --setup` once to fetch "
                    "whisper, then dictation works fully offline."), true);
        else
            note(tr("Ready — press to talk. Try \"tell coder 2 to add tests\", \"start a crew to "
                    "refactor the parser\", or \"type git status\"."));
    }

private:
    void note(const QString& msg, bool warn = false) {
        const Theme::Colors c = Theme::currentColors();
        log_->appendHtml(QStringLiteral("<span style='color:%1'>%2</span>")
                             .arg((warn ? c.warn : c.dim).name(), msg.toHtmlEscaped()));
    }

    void toggle() {
        if (recording_) {
            stopAndTranscribe();
            return;
        }
        if (!SttRecorder::canRecord()) {
            host_.setStatus(tr("no microphone recorder installed"));
            return;
        }
        QString err;
        if (!rec_->start(&err)) {
            note(tr("could not start recording: %1").arg(err), true);
            return;
        }
        recording_ = true;
        ptt_->setText(tr("● Listening… tap to stop"));
        host_.setStatus(tr("listening…"));
    }

    void stopAndTranscribe() {
        recording_ = false;
        ptt_->setText(tr("Press to talk"));
        const QString wav = rec_->stop();
        if (wav.isEmpty()) {
            note(tr("nothing captured"), true);
            return;
        }
        ptt_->setEnabled(false);
        host_.setStatus(tr("transcribing…"));
        // transcribe() blocks (drives a local whisper); run it off the UI thread and
        // marshal the text back. QPointer means a closed pane just drops the result.
        QPointer<VoiceWidget> self = this;
        std::thread([self, wav] {
            QString err;
            const QString text = Stt::transcribe(wav, &err);
            QMetaObject::invokeMethod(qApp, [self, text, err] {
                if (!self) return;
                self->ptt_->setEnabled(true);
                if (text.trimmed().isEmpty()) {
                    self->note(self->tr("transcription failed: %1")
                                   .arg(err.isEmpty() ? self->tr("no text") : err), true);
                    self->host_.setStatus(self->tr("transcription failed"));
                    return;
                }
                self->handle(text.trimmed());
            });
        }).detach();
    }

    void handle(const QString& raw) {
        heard_->setText(QStringLiteral("“%1”").arg(raw));
        const QString lc = raw.toLower();

        // Explicit dictation prefix → straight to a terminal (unless in Command mode).
        static const QRegularExpression pre(
            QStringLiteral("^(type|say|send|tell|dictate|prompt)\\b[:,]?\\s*"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch pm = pre.match(raw);
        if (pm.hasMatch() && mode_ != 1 &&
            !lc.contains(QStringLiteral("coder")) && !lc.contains(QStringLiteral("crew"))) {
            toCli(raw.mid(pm.capturedLength()));
            return;
        }

        if (mode_ != 2 && tryCommand(raw)) return;   // not in forced "To terminal" mode
        if (mode_ == 1) {                             // Command-only: no CLI fallback
            note(tr("no command matched: %1").arg(raw), true);
            host_.setStatus(tr("no matching command"));
            return;
        }
        toCli(raw);  // Auto / To-terminal fallthrough
    }

    bool tryCommand(const QString& raw) {
        // "tell/steer/have/ask coder N to X" → steer one coder.
        static const QRegularExpression coder(
            QStringLiteral("\\b(?:tell|steer|have|ask)\\s+coder\\s+(\\d+)\\s+(?:to\\s+)?(.+)$"),
            QRegularExpression::CaseInsensitiveOption);
        if (auto m = coder.match(raw); m.hasMatch()) {
            steer(m.captured(1).toInt(), m.captured(2).trimmed());
            return true;
        }
        // "tell/steer the crew/team/everyone to X" → steer the whole crew.
        static const QRegularExpression crew(
            QStringLiteral("\\b(?:tell|steer|have|ask)\\s+(?:the\\s+)?(?:crew|team|everyone|all "
                           "coders)\\s+(?:to\\s+)?(.+)$"),
            QRegularExpression::CaseInsensitiveOption);
        if (auto m = crew.match(raw); m.hasMatch()) {
            steer(0, m.captured(1).trimmed());
            return true;
        }
        // "start/launch/spin up a crew to X" → launch a crew in a terminal.
        static const QRegularExpression start(
            QStringLiteral("\\b(?:start|launch|spin up|kick off|run|create|fire up)\\s+(?:a\\s+|the"
                           "\\s+|new\\s+)?(?:crew|team)\\b\\s*(?:to|for|that|:)?\\s*(.+)$"),
            QRegularExpression::CaseInsensitiveOption);
        if (auto m = start.match(raw); m.hasMatch()) {
            const QString task = m.captured(1).trimmed();
            host_.runInTerminal(QStringLiteral("ollamadev crew %1").arg(shq(task)));
            note(tr("launching a crew: %1").arg(task));
            host_.setStatus(tr("launching crew"));
            return true;
        }
        return false;
    }

    void steer(int coder, const QString& msg) {
        QString err;
        if (Crew::steer(coder, msg, &err)) {
            note(coder == 0 ? tr("steered the crew: %1").arg(msg)
                            : tr("steered coder %1: %2").arg(coder).arg(msg));
            host_.setStatus(coder == 0 ? tr("steered the crew") : tr("steered coder %1").arg(coder));
        } else {
            note(tr("steer failed: %1").arg(err), true);
        }
    }

    void toCli(const QString& text) {
        const QString t = text.trimmed();
        if (t.isEmpty()) return;
        host_.runInTerminal(t);
        note(tr("→ terminal: %1").arg(t));
        host_.setStatus(tr("sent to terminal"));
    }

    PaneHost& host_;
    SttRecorder* rec_ = nullptr;
    QPushButton* ptt_ = nullptr;
    QLabel* heard_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    bool recording_ = false;
    int mode_ = 0;  // 0 Auto · 1 Command · 2 To terminal
};

}  // namespace

PaneSpec makeVoicePaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("voice");
    s.title = QStringLiteral("Voice");
    s.group = QStringLiteral("Tools");
    s.singleton = true;
    s.factory = [](PaneHost& host) -> QWidget* { return new VoiceWidget(host); };
    return s;
}

}  // namespace odv
