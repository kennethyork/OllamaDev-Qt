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

    odv::MainWindow w;
    w.show();
    return app.exec();
}
