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

#include "Config.h"
#include "MainWindow.h"
#include "Theme.h"
#include "Tools.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("OllamaDev");
    QCoreApplication::setApplicationName("OllamaDev ADE");


    odv::Config::load();
    odv::Tools::registerAll();
    odv::Theme::apply(&app, odv::Theme::current());

    // An explicit folder on the command line (the .desktop file's `%F`, or a path
    // typed after the binary) wins. With none — the usual menu launch, whose cwd is
    // just $HOME — the window falls back to the last active workspace instead.
    const QStringList args = app.arguments();
    const QString startupPath = args.size() > 1 ? args.at(1) : QString();

    odv::MainWindow w(startupPath);
    w.show();
    return app.exec();
}
