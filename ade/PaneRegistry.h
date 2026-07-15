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
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <QWidget>
#include <functional>

#include "PaneHost.h"

namespace odv {

// One kind of canvas pane. `factory` builds the content widget; the chrome
// (title bar, close-confirm, drag/resize) is added by the canvas, so a pane
// author only writes the content.
struct PaneSpec {
    QString kind;    // stable id; also the singleton pane's id
    QString title;   // menu label and pane title
    QString group;   // menu section: "" | "Views" | "Crew" | "Tools"
    bool singleton = true;  // true → one instance, re-raised; false → many
    std::function<QWidget*(PaneHost&)> factory;

    // Optional session persistence for the pane's INNER content (a chat
    // transcript, a search query, …). `snapshot` serialises the content widget
    // the factory built; `restore` re-applies that blob to a freshly built one.
    // Leave both null and the pane simply reopens fresh at its saved geometry —
    // which is right for a live/derived view (git, graph, topology) that has no
    // user-entered state to carry.
    std::function<QJsonObject(QWidget*)> snapshot;
    std::function<void(QWidget*, const QJsonObject&)> restore;
};

// Extra panes beyond the four built into MainWindow (terminal/board/editor/
// files/settings). New panes register here so MainWindow needs no edit — its
// Add menu and dispatch iterate the registry.
class PaneRegistry {
public:
    static PaneRegistry& instance();

    void add(const PaneSpec& spec);
    const QVector<PaneSpec>& all() const { return specs_; }
    const PaneSpec* find(const QString& kind) const;

private:
    QVector<PaneSpec> specs_;
};

// Populates the registry with every extra pane. Defined in Panes.cpp; each pane
// file contributes one line. Called once at startup.
void registerExtraPanes();

}  // namespace odv
