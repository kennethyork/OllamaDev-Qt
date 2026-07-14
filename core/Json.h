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
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace odv {
namespace json {

// Cloud models wrap JSON in ```json fences even when asked for raw JSON, and
// some models prepend prose. decodeLoose finds the payload anyway: strip
// fences, then fall back to the outermost balanced {...} or [...].
QJsonDocument decodeLoose(const QString& text);

QJsonObject objectFrom(const QString& text);

QByteArray encode(const QJsonObject& o);
QByteArray encode(const QJsonArray& a);

// Walk a dotted path ("crew.coderModel") through nested objects.
QJsonValue at(const QJsonObject& root, const QString& dottedKey,
              const QJsonValue& fallback = {});

// Deep merge `overlay` onto `base` (objects merge, scalars overwrite).
QJsonObject mergeDeep(const QJsonObject& base, const QJsonObject& overlay);

// Expand a flat {"a.b": 1} map into nested {"a":{"b":1}}.
QJsonObject expandDotted(const QJsonObject& flat);

}  // namespace json
}  // namespace odv
