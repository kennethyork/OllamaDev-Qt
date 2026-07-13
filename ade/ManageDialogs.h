#pragma once
#include "PaneHost.h"

namespace odv {

// Modal management UIs: roles, skills, hooks, the crew-launch dialog and the
// review/diff dialog. STUB — replace with the real dialogs.
namespace ManageDialogs {
void openRoles(PaneHost& host);
void openSkills(PaneHost& host);
void openHooks(PaneHost& host);
void openCrewLaunch(PaneHost& host);
void openReview(PaneHost& host);
}  // namespace ManageDialogs

}  // namespace odv
