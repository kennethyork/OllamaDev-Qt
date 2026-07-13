#pragma once
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
