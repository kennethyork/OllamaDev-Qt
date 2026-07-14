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
#include <QWidget>

namespace odv {

// The narrow surface a pane is allowed to reach back into the app through.
// Panes get this instead of a MainWindow* so a new pane cannot entangle itself
// with MainWindow internals — which is what lets several of them be written
// independently and dropped in via the registry.
class PaneHost {
public:
    virtual ~PaneHost() = default;

    virtual QString project() const = 0;          // the project root (the app's cwd)
    virtual QString currentModel() const = 0;
    virtual QString currentBackend() const = 0;

    virtual void setStatus(const QString& msg) = 0;
    virtual void openFile(const QString& path) = 0;            // route to the editor pane
    virtual void runInTerminal(const QString& command) = 0;    // spawn a terminal running a command
    virtual QWidget* window() = 0;                             // parent for modal dialogs
};

}  // namespace odv
