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
#include "PaneHost.h"

namespace odv {

// Modal management UIs, ported from app.js (Roles/SkillMgr/HookMgr/openCrew/Diff).
// Each opens a QDialog parented to host.window() and talks to the core managers
// directly (CrewRoles/Skills/Hooks/GitFlow) — the desktop links odv-core, so
// there is no reason to shell out for what is a plain function call.
namespace ManageDialogs {
void openRoles(PaneHost& host);       // crew role personas — list / add / remove
void openSkills(PaneHost& host);      // reusable skills — list / edit / save / remove
void openHooks(PaneHost& host);       // lifecycle shell hooks — list / add / remove
void openCrewLaunch(PaneHost& host);  // crew setup form → runInTerminal("ollamadev crew …")
void openReview(PaneHost& host);      // the working-tree diff, +/- coloured
}  // namespace ManageDialogs

}  // namespace odv
