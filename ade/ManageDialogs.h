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
