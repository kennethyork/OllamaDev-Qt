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

class QWidget;

namespace odv {

// The theme editor — a port of the PHP app's "theme engine" (app.js ~3540):
// one colour field per role, a live preview that repaints the app as you type,
// and Save, which persists an editable custom theme (Theme::saveCustom) and
// applies it. open() is modal against `parent` (the main window).
class ThemeDialog {
public:
    static void open(QWidget* parent);
};

}  // namespace odv
