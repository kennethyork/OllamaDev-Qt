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
