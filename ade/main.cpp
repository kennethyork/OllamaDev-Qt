// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <QApplication>

#include <csignal>

#include "Config.h"
#include "MainWindow.h"
#include "Theme.h"
#include "Tools.h"

int main(int argc, char** argv) {
    // A GUI must outlive the terminal it was launched from. Launch the ADE from a
    // shell in its own repo, then run an agent/crew (or a build, or just close the
    // tab) and that shell's session delivers SIGHUP — whose DEFAULT action is to
    // terminate, taking the whole canvas with it. Ignore it: the window's ✕ and
    // Quit are the only ways to close the app. SIGTERM is left alone so a normal
    // `kill` still works, and the atomic autosave means even that keeps the layout.
    ::signal(SIGHUP, SIG_IGN);

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("OllamaDev");
    QCoreApplication::setApplicationName("OllamaDev ADE");


    odv::Config::load();
    odv::Tools::registerAll();
    odv::Theme::apply(&app, odv::Theme::current());

    // Window sizing. On a normal desktop we open at the window's own size, but a
    // VPS/remote-desktop deployment runs on a headless X of a fixed size and wants
    // the canvas to fill it — hence --fullscreen (kiosk) and --maximized, also
    // switchable from a systemd unit via ODV_FULLSCREEN=1 / ODV_MAXIMIZED=1.
    enum class Show { Normal, Maximized, Fullscreen } showMode = Show::Normal;
    if (qEnvironmentVariableIntValue("ODV_FULLSCREEN") == 1) showMode = Show::Fullscreen;
    else if (qEnvironmentVariableIntValue("ODV_MAXIMIZED") == 1) showMode = Show::Maximized;

    // An explicit folder on the command line (the .desktop file's `%F`, or a path
    // typed after the binary) wins. With none — the usual menu launch, whose cwd is
    // just $HOME — the window falls back to the last active workspace instead.
    // Flags are stripped first so a `--fullscreen` is never mistaken for a path.
    QString startupPath;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString a = args.at(i);
        if (a == QLatin1String("--fullscreen") || a == QLatin1String("--kiosk"))
            showMode = Show::Fullscreen;
        else if (a == QLatin1String("--maximized"))
            showMode = Show::Maximized;
        else if (!a.startsWith(QLatin1String("--")) && startupPath.isEmpty())
            startupPath = a;
    }

    odv::MainWindow w(startupPath);
    switch (showMode) {
        case Show::Fullscreen: w.showFullScreen(); break;
        case Show::Maximized:  w.showMaximized();  break;
        case Show::Normal:     w.show();           break;
    }
    return app.exec();
}
