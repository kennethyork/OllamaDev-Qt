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
