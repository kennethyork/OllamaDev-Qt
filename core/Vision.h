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
#include <QString>
#include <QStringList>

#include "Backend.h"

namespace odv {

// VISION — how a picture reaches the model.
//
// The user writes `@shot.png` or `/image shot.png`; we base64 the file onto the
// message's images[] (which Ollama hands to a multimodal model, and ignores on a
// text one) and take the token back out of the prompt, so what the model reads is
// a natural sentence and not a path it will try to open with a tool.
class Vision {
public:
    // The one call every surface should use: the REPL, a one-shot `ollamadev "…"`,
    // a desktop chat pane. It attaches the images and returns the cleaned prompt.
    //
    // Anything that builds a user message out of typed text goes through here.
    // Vision is a property of a PROMPT, not of the CLI — and every surface
    // re-implementing it is exactly how one of them ends up silently not having it
    // (which is what had happened to the one-shot path).
    //
    // `attached`, when given, receives how many images actually made it on: a path
    // that does not exist, or is not an image, is left alone as ordinary text.
    static QString attach(ChatMessage& msg, const QString& text, int* attached = nullptr);

    static QStringList extractImagePaths(const QString& prompt);  // @img.png, /image path
    static QString stripImageTokens(const QString& prompt);
    static QString encodeBase64(const QString& path, QString* err = nullptr);  // "" on failure
};

}  // namespace odv
