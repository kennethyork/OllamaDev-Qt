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
#include <QDialog>

#include "PaneHost.h"

namespace odv {

// "Tidy history" — interactive rebase with an AI that proposes the plan.
//
// The model reads your commits and suggests which to fold together, which to
// reword and which to drop. You see EVERY line of that plan and can change any of
// it before a single commit is rewritten. That review step is the feature: an AI
// that silently rewrote your history would be a liability, not a tool.
class RebaseDialog : public QDialog {
public:
    RebaseDialog(PaneHost& host, QWidget* parent = nullptr);
};

}  // namespace odv
