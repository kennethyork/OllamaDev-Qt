// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ChatPane.h"

#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QTextEdit>
#include <QVBoxLayout>

#include <thread>

#include "Agent.h"
#include "Backend.h"
#include "Config.h"
#include "Session.h"
#include "Theme.h"
#include "Vision.h"

namespace odv {
namespace {

// A CHAT pane: a plain conversation, no tools, no file edits.
//
// Deliberately not an agent. The agent already has a home (the terminal, the
// crew); what was missing on the canvas was somewhere to just *ask something* —
// paste an error, show it a screenshot, argue about an approach — without a model
// deciding to go and edit your files about it. That is why no tool schemas are
// sent here, which as a bonus is what lets a VISION model work: most of them ship
// with no `tools` capability and answer an empty string if you hand them one.
//
// Each pane is its own Session (persisted, resumable) and its own model, so two
// chats side by side genuinely are two conversations.
class ChatWidget : public QWidget {
public:
    explicit ChatWidget(PaneHost& host, QWidget* parent = nullptr)
        : QWidget(parent), host_(host) {
        setMinimumSize(360, 300);
        backendId_ = host.currentBackend();

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(8, 8, 8, 8);
        root->setSpacing(6);

        // --- bar: model, attach, clear -------------------------------------
        auto* bar = new QHBoxLayout;
        model_ = new QComboBox(this);
        model_->setEditable(true);  // a tag we have not listed is still a valid tag
        if (auto be = Backends::get(backendId_)) model_->addItems(be->models());
        model_->setCurrentText(host.currentModel());

        auto* attach = new QPushButton(tr("Image"), this);
        attach->setToolTip(tr("Attach an image (a vision model will see it)"));
        attach->setFixedWidth(56);

        auto* clear = new QPushButton(tr("New"), this);
        clear->setToolTip(tr("Start a fresh conversation"));

        bar->addWidget(model_, 1);
        bar->addWidget(attach);
        bar->addWidget(clear);
        root->addLayout(bar);

        // --- transcript -----------------------------------------------------
        view_ = new QTextEdit(this);
        view_->setReadOnly(true);
        view_->setFrameShape(QFrame::NoFrame);
        root->addWidget(view_, 1);

        status_ = new QLabel(this);
        status_->setStyleSheet(
            QStringLiteral("color:%1").arg(Theme::currentColors().faint.name()));
        root->addWidget(status_);

        // --- input ----------------------------------------------------------
        auto* row = new QHBoxLayout;
        input_ = new QPlainTextEdit(this);
        input_->setPlaceholderText(tr("Ask anything…  (@shot.png attaches an image)"));
        input_->setMaximumHeight(72);
        send_ = new QPushButton(tr("Send"), this);
        send_->setProperty("cta", true);
        row->addWidget(input_, 1);
        row->addWidget(send_);
        root->addLayout(row);

        connect(send_, &QPushButton::clicked, this, [this] { send(); });
        connect(attach, &QPushButton::clicked, this, [this] { pickImage(); });
        connect(clear, &QPushButton::clicked, this, [this] { reset(); });

        session_ = Session::create(host.project());
        replay();
    }

protected:
    // Enter sends; Shift+Enter is a newline. A chat box that needs a mouse click to
    // send is a chat box nobody uses.
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == input_ && e->type() == QEvent::KeyPress) {
            auto* k = static_cast<QKeyEvent*>(e);
            if ((k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) &&
                !(k->modifiers() & Qt::ShiftModifier)) {
                send();
                return true;
            }
        }
        return QWidget::eventFilter(o, e);
    }

    void showEvent(QShowEvent* e) override {
        QWidget::showEvent(e);
        input_->installEventFilter(this);
        input_->setFocus();
    }

private:
    void append(const QString& who, const QString& text, const QColor& c) {
        // QTextEdit::append starts a NEW paragraph; insertHtml at the cursor merges
        // into the previous one, which ran the user's message and the model's name
        // together on a single line ("higpt-oss:20b-cloud").
        view_->append(QStringLiteral("<b style='color:%1'>%2</b><br>%3")
                          .arg(c.name(), who, text.toHtmlEscaped().replace('\n', "<br>")));
        view_->verticalScrollBar()->setValue(view_->verticalScrollBar()->maximum());
    }

    void replay() {
        view_->clear();
        const Theme::Colors c = Theme::currentColors();
        for (const ChatMessage& m : session_.messages()) {
            if (m.role == QLatin1String("user"))
                append(tr("You"), m.content, c.accent);
            else if (m.role == QLatin1String("assistant"))
                append(model_->currentText(), m.content, c.fg);
        }
    }

    void reset() {
        if (busy_) return;
        session_ = Session::create(host_.project());
        view_->clear();
        status_->clear();
    }

    void pickImage() {
        const QString p = QFileDialog::getOpenFileName(
            host_.window(), tr("Attach an image"), host_.project(),
            tr("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp)"));
        if (p.isEmpty()) return;
        // Drop it in as an @token: exactly what a user would type, and it goes down
        // the one Vision::attach path every other surface uses.
        input_->insertPlainText(QStringLiteral("@%1 ").arg(p));
        input_->setFocus();
    }

    void send() {
        if (busy_) return;
        const QString text = input_->toPlainText().trimmed();
        if (text.isEmpty()) return;
        const QString model = model_->currentText().trimmed();
        if (model.isEmpty()) {
            status_->setText(tr("Pick a model first."));
            return;
        }
        auto be = Backends::get(backendId_);
        if (!be) {
            status_->setText(tr("No backend."));
            return;
        }

        ChatMessage user;
        user.role = QStringLiteral("user");
        int images = 0;
        user.content = Vision::attach(user, text, &images);
        session_.messages().append(user);
        session_.save();

        append(tr("You"),
               images > 0 ? QStringLiteral("%1\n[%2 image(s)]").arg(user.content).arg(images)
                          : user.content,
               Theme::currentColors().accent);
        input_->clear();

        // A model with no `tools` capability is fine here — we send no tools at all.
        // Say so anyway when an image is attached, because "why did my vision model
        // answer nothing" is a question worth never having to ask.
        setBusy(true);
        status_->setText(tr("thinking…"));

        // Start the assistant's paragraph now and stream into it, so the first token
        // appears the moment it arrives instead of after the whole reply lands.
        append(model, QString(), Theme::currentColors().fg);
        streaming_.clear();

        QPointer<ChatWidget> self = this;
        QVector<ChatMessage> msgs = session_.messages();
        std::thread([self, be, model, msgs]() mutable {
            StreamSink sink;
            sink.onContent = [self](const QString& chunk) {
                QMetaObject::invokeMethod(qApp, [self, chunk] {
                    if (self) self->onChunk(chunk);
                });
            };
            CancelToken cancel;
            const ChatTurn t = be->chat(model, msgs, QJsonArray(), sink, cancel);
            QMetaObject::invokeMethod(qApp, [self, t] {
                if (self) self->onDone(t);
            });
        }).detach();
    }

    void onChunk(const QString& chunk) {
        streaming_ += chunk;
        view_->moveCursor(QTextCursor::End);
        view_->insertPlainText(chunk);
        view_->verticalScrollBar()->setValue(view_->verticalScrollBar()->maximum());
    }

    void onDone(const ChatTurn& t) {
        setBusy(false);
        // A non-streaming backend hands the whole reply back at the end, so if the
        // sink never fired, paint it now.
        if (streaming_.isEmpty() && !t.content.isEmpty()) onChunk(t.content);

        if (!t.ok || (streaming_.isEmpty() && t.content.isEmpty())) {
            status_->setText(t.error.isEmpty() ? tr("The model returned nothing.") : t.error);
            return;
        }
        status_->clear();
        ChatMessage m;
        m.role = QStringLiteral("assistant");
        m.content = streaming_.isEmpty() ? t.content : streaming_;
        session_.messages().append(m);
        session_.save();
    }

    void setBusy(bool on) {
        busy_ = on;
        send_->setEnabled(!on);
        input_->setEnabled(!on);
    }

    PaneHost& host_;
    QString backendId_;
    Session session_ = Session::create(QString());
    QComboBox* model_;
    QTextEdit* view_;
    QPlainTextEdit* input_;
    QPushButton* send_;
    QLabel* status_;
    QString streaming_;
    bool busy_ = false;
};

}  // namespace

PaneSpec makeChatPaneSpec() {
    PaneSpec s;
    s.kind = QStringLiteral("chat");
    s.title = QStringLiteral("Chat");
    s.group = QStringLiteral("Views");
    // The FIRST non-singleton pane. Two chats side by side is the whole point: one
    // arguing about an approach, one reading a stack trace.
    s.singleton = false;
    s.factory = [](PaneHost& h) -> QWidget* { return new ChatWidget(h); };
    return s;
}

}  // namespace odv
