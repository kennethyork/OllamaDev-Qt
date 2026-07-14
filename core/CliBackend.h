// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <QMutex>
#include <QString>
#include <QStringList>

#include "Backend.h"

namespace odv {

// A headless agentic coding CLI driven as a subprocess: Claude Code, Codex,
// Gemini CLI, Cursor Agent, OpenCode, Qwen Code, Aider, Goose, Amp, Crush,
// Droid. One class, parameterised by id — they differ only in argv.
//
// These are NOT chat completion endpoints. Each one is a complete agent: it
// gets a prompt, then reads, greps, edits and runs commands in the working
// directory on its own, and prints a final answer. That is why
// supportsNativeTools() is false for all of them (see below) and why chat()
// returns an empty `calls` list: there is no tool loop for us to run, the CLI
// already ran it.
class CliBackend : public IModelBackend {
public:
    explicit CliBackend(QString backendId);

    // Every CLI we know how to drive, installed or not.
    static QStringList ids();
    static QString labelFor(const QString& backendId);

    QString id() const override { return id_; }
    QString label() const override { return labelFor(id_); }

    bool available() override;
    QStringList models() override;
    QString defaultModel() override;

    // ALWAYS false. Each CLI runs its OWN agent loop and does its own file
    // edits, so the crew must hand it one self-contained prompt and let it
    // work. Feeding it our tool schemas would be asking a coworker to narrate
    // every keystroke back to us — it has no such interface, and pretending it
    // does would strand the crew waiting for tool calls that never come.
    bool supportsNativeTools() const override { return false; }

    ChatTurn chat(const QString& model,
                  const QVector<ChatMessage>& messages,
                  const QJsonArray& toolSchemas,
                  const StreamSink& sink,
                  const CancelToken& cancel) override;

    QJsonObject chatJson(const QString& model,
                         const QVector<ChatMessage>& messages,
                         const CancelToken& cancel) override;

    int concurrencyLimit(const QString& model) override;

private:
    // The full argv for one headless run, or empty when the binary is missing.
    QStringList argv(const QString& model, const QString& prompt,
                     const QString& lastMessageFile) const;

    // Some CLIs take the prompt on stdin (claude -p, codex exec -), others as
    // an argument. Getting this backwards hangs the process forever waiting on
    // a stdin that never closes.
    bool promptOnStdin() const;

    // codex writes a clean final answer to a file with -o; everything else we
    // reconstruct from stdout.
    bool usesLastMessageFile() const;

    QString executable();
    static QStringList extraSearchDirs();
    static QString flatten(const QVector<ChatMessage>& messages);
    static QString stripAnsi(const QString& s);

    QString id_;
    QString command_;

    QMutex probeMutex_;
    bool probed_ = false;
    QString exePath_;
};

}  // namespace odv
