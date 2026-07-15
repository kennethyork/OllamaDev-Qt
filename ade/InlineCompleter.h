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
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace odv {

// Fill-in-the-middle inline completion, the ghost-text kind Copilot/Cursor made
// standard. Async and non-blocking: it runs on the GUI thread's event loop via
// QNetworkAccessManager, so a slow model never freezes typing. Only ONE request
// is ever in flight — a new one aborts the previous, because the only suggestion
// that matters is the one for what's under the cursor right now.
//
// It uses Ollama's native FIM: POST /api/generate with `prompt` (the code BEFORE
// the cursor) and `suffix` (the code AFTER), which a FIM-capable coder model
// (qwen2.5-coder, deepseek-coder, codellama, starcoder2, codegemma…) splices with
// its own hole tokens. A model without FIM simply returns nothing useful, so the
// feature degrades to "no ghost" rather than misbehaving.
class InlineCompleter : public QObject {
    Q_OBJECT
public:
    explicit InlineCompleter(QObject* parent = nullptr);

    // On unless editor.autocomplete is explicitly false. The model is
    // editor.autocomplete.model (default "qwen2.5-coder"); if it isn't installed
    // the request just comes back empty, which is the correct silent no-op.
    static bool enabled();
    static QString model();

    // Ask for a completion at the cursor. Aborts any in-flight request first.
    void request(const QString& prefix, const QString& suffix);
    void cancel();

signals:
    // The suggested continuation (may be empty == nothing to offer). Carries the
    // request id so a stale reply that lands after the cursor moved is dropped.
    void suggestion(const QString& text);

private:
    QNetworkAccessManager* nam_;
    QNetworkReply* reply_ = nullptr;
};

}  // namespace odv
